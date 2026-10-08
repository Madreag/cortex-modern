#include "GnsTransport.h"
#include "DiagnosticLine.h"
#include "NetIceServers.h"
#include "NetLobbyProtocol.h"
#include "SettingsMan.h"
#include "System.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <utility>
#include <iostream>
#include <sstream>
#include <string_view>
#include <functional>
#include <cmath>
#include <cstdlib>

#ifdef CCCP_WITH_GNS
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <steam/steamnetworkingsockets.h>
// A relayed route dies once its TURN permission lapses unless the library refreshes it.
#if !defined(STEAMNETWORKINGSOCKETS_TURN_LIFETIME) || STEAMNETWORKINGSOCKETS_TURN_LIFETIME < 2 || !defined(STEAMNETWORKINGSOCKETS_ICE_CANDIDATE_POLICY) || STEAMNETWORKINGSOCKETS_ICE_CANDIDATE_POLICY < 2 || !defined(STEAMNETWORKINGSOCKETS_TURN_STREAMS) || STEAMNETWORKINGSOCKETS_TURN_STREAMS < 2 || !defined(STEAMNETWORKINGSOCKETS_NETWORK_RECOVERY)
#error "GameNetworkingSockets without external/patches/gns-turn-lifetime.patch; build it into <GNS_ROOT>-turnfix, see docs/turn-relay.md"
#endif
#endif

namespace RTE {
	bool ApplyCrossTransportFault(int lagMs, float lossPercent, float jitterMs, uint64_t durationMs);
	uint64_t NetLockstepSharedClockMs();
	static std::atomic<uint64_t> s_CrossTransportResetMs{0};

	std::string GnsTransport::TurnHostReceipts(const std::string& servers) {
		std::istringstream input(servers);
		std::string server, result;
		while (std::getline(input, server, ',')) {
			if (server.empty()) continue;
			if (server.starts_with("turns:")) server.erase(0, 6);
			else if (server.starts_with("turn:")) server.erase(0, 5);
			if (const auto query = server.find('?'); query != std::string::npos) server.resize(query);
			std::string host = server;
			if (server.front() == '[') {
				const auto end = server.find(']');
				if (end == std::string::npos) return {};
				host = server.substr(1, end - 1);
			} else if (std::count(server.begin(), server.end(), ':') == 1) host = server.substr(0, server.find(':'));
			if (!result.empty()) result += ',';
			result += System::Sha256Hex(host.data(), host.size());
		}
		return result;
	}

	namespace {
		void SetError(std::string* error, const std::string& message) {
			if (error) {
				*error = message;
			}
		}
	}

#ifdef CCCP_WITH_GNS

	namespace {
		struct UplinkStallConfig { uint64_t frame = 0, durationMs = 0; };
		UplinkStallConfig ParseUplinkStall(const char* frame, const char* duration) {
			const auto number = [](const char* text, uint64_t limit) {
				if (!text || !*text) return uint64_t(0);
				uint64_t value = 0;
				for (const char* at = text; *at; ++at) {
					if (*at < '0' || *at > '9' || value > limit / 10) return uint64_t(0);
					value = value * 10 + static_cast<uint64_t>(*at - '0');
					if (value > limit) return uint64_t(0);
				}
				return value;
			};
			UplinkStallConfig result{number(frame, 1000000), number(duration, 10000)};
			return result.frame && result.durationMs ? result : UplinkStallConfig{};
		}
		const UplinkStallConfig& TestUplinkStallConfig() {
			static const UplinkStallConfig config = [] {
				const char* headless = std::getenv("CCCP_HEADLESS");
				return headless && std::string_view(headless) == "1" ? ParseUplinkStall(std::getenv("CC_TEST_GNS_UPLINK_STALL_FRAME"), std::getenv("CC_TEST_GNS_UPLINK_STALL_MS")) : UplinkStallConfig{};
			}();
			return config;
		}
		float GlobalLoss(ESteamNetworkingConfigValue key) {
			float value = 0; size_t size = sizeof(value); ESteamNetworkingConfigDataType type = k_ESteamNetworkingConfig_Float;
			SteamNetworkingUtils()->GetConfigValue(key, k_ESteamNetworkingConfig_Global, 0, &type, &value, &size);
			return value;
		}
		struct UplinkStall {
			bool started = false, finished = false;
			uint64_t firstMs = 0, untilMs = 0;
			float previousLoss = 0;
			bool NoteFrame(const UplinkStallConfig& config, uint64_t frame, uint64_t nowMs) {
				if (started || config.durationMs == 0 || frame < config.frame) return false;
				previousLoss = GlobalLoss(k_ESteamNetworkingConfig_FakePacketLoss_Send);
				if (!SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 100.0F)) return false;
				started = true; firstMs = nowMs; untilMs = nowMs + config.durationMs;
				DiagnosticLine() << "[test-uplink-stall] begin frame=" << frame << " duration_ms=" << config.durationMs << " send_loss=100 recv_loss=" << GlobalLoss(k_ESteamNetworkingConfig_FakePacketLoss_Recv)
				                 << " clock_ms=" << NetLockstepSharedClockMs() << std::endl;
				return true;
			}
			void Update(uint64_t nowMs) {
				if (!started || finished) return;
				if (nowMs < untilMs) { SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, 100.0F); return; }
				if (!SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, previousLoss)) return;
				finished = true;
				DiagnosticLine() << "[test-uplink-stall] end elapsed_ms=" << nowMs - firstMs << " send_loss=" << previousLoss
				                 << " clock_ms=" << NetLockstepSharedClockMs() << std::endl;
			}
		};
		UplinkStall s_TestUplinkStall;
		uint64_t UplinkStallClockMs() { return static_cast<uint64_t>(SteamNetworkingUtils()->GetLocalTimestamp() / 1000); }

		struct SignalField { uint32_t number; uint8_t wire; uint64_t integer; std::string_view bytes; };
		bool SignalFields(std::string_view bytes, std::vector<SignalField>& out) {
			size_t cursor = 0;
			const auto varint = [&](uint64_t& value) {
				value = 0;
				for (unsigned shift = 0; shift < 64 && cursor < bytes.size(); shift += 7) {
					const uint8_t byte = static_cast<uint8_t>(bytes[cursor++]);
					if (shift == 63 && byte > 1) return false;
					value |= uint64_t(byte & 127) << shift;
					if (!(byte & 128)) return true;
				}
				return false;
			};
			while (cursor < bytes.size()) {
				uint64_t tag = 0; if (!varint(tag) || tag < 8 || tag >> 3 > UINT32_MAX) return false;
				SignalField field{static_cast<uint32_t>(tag >> 3), static_cast<uint8_t>(tag & 7), 0, {}};
				if (field.wire == 0) { if (!varint(field.integer)) return false; }
				else if (field.wire == 1 || field.wire == 5) {
					const size_t count = field.wire == 1 ? 8 : 4; if (count > bytes.size() - cursor) return false;
					for (size_t i = 0; i < count; ++i) field.integer |= uint64_t(static_cast<uint8_t>(bytes[cursor++])) << (8 * i);
				} else if (field.wire == 2) {
					uint64_t count = 0; if (!varint(count) || count > bytes.size() - cursor) return false;
					field.bytes = bytes.substr(cursor, static_cast<size_t>(count)); cursor += static_cast<size_t>(count);
				} else return false;
				out.push_back(field);
			}
			return true;
		}
		struct CandidateEvidence { std::string identity; uint32_t connection = 0; std::map<std::string, std::string> candidates; };
		CandidateEvidence ReadCandidateEvidence(std::string_view signal) {
			CandidateEvidence result;
			if (signal.size() > 1024 * 1024) return result;
			std::vector<SignalField> root;
			if (!SignalFields(signal, root)) return result;
			for (const auto& field: root) {
				if (field.number == 8 && field.wire == 2 && field.bytes.size() <= 256) result.identity = field.bytes;
				if (field.number == 9 && field.wire == 5) result.connection = static_cast<uint32_t>(field.integer);
				if (field.number != 13 || field.wire != 2) continue;
				std::vector<SignalField> reliable; if (!SignalFields(field.bytes, reliable)) continue;
				for (const auto& message: reliable) if (message.number == 1 && message.wire == 2) {
					std::vector<SignalField> ice; if (!SignalFields(message.bytes, ice)) continue;
					for (const auto& addition: ice) if (addition.number == 1 && addition.wire == 2) {
						std::vector<SignalField> candidate; if (!SignalFields(addition.bytes, candidate)) continue;
						for (const auto& attribute: candidate) if (attribute.number == 3 && attribute.wire == 2 && attribute.bytes.size() <= 512) {
							std::istringstream words{std::string(attribute.bytes)};
							std::string foundation, protocol, address, marker, type; uint32_t component = 0, priority = 0, port = 0;
							if (!(words >> foundation >> component >> protocol >> priority >> address >> port >> marker >> type) || !foundation.starts_with("candidate:") || protocol != "udp" || port == 0 || port > 65535 || marker != "typ" ||
							    (type != "host" && type != "srflx" && type != "prflx" && type != "relay")) continue;
							SteamNetworkingIPAddr endpoint{};
							const std::string text = (address.find(':') == std::string::npos ? address : "[" + address + "]") + ":" + std::to_string(port);
							if (!endpoint.ParseString(text.c_str())) continue;
							char canonical[SteamNetworkingIPAddr::k_cchMaxString]{}; endpoint.ToString(canonical, sizeof(canonical), true);
							if (result.candidates.size() < 64) result.candidates[canonical] = type;
						}
					}
				}
			}
			return result;
		}
		bool g_GnsInitialized = false;
		uint32_t g_GnsRefCount = 0;
		std::atomic<uint64_t> g_GnsLingerUntilMs{0};

		uint64_t GnsNowMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		void KeepGnsForLingeringClose() {
			const uint64_t deadline = GnsNowMs() + 100;
			uint64_t prior = g_GnsLingerUntilMs.load();
			while (prior < deadline && !g_GnsLingerUntilMs.compare_exchange_weak(prior, deadline)) {}
		}


		bool AcquireGns(std::string* error) {
			if (!g_GnsInitialized) {
				// Every migration listener needs a distinct identity before any socket opens.
				std::random_device random;
				std::string name = "p-";
				for (int i = 0; i < 28; ++i) name += "0123456789abcdef"[random() & 15];
				SteamNetworkingIdentity identity;
				identity.SetGenericString(name.c_str());
				SteamDatagramErrMsg initError;
				if (!GameNetworkingSockets_Init(&identity, initError)) {
					SetError(error, std::string("GameNetworkingSockets_Init failed: ") + initError);
					return false;
				}
				g_GnsInitialized = true;
			}
			++g_GnsRefCount;
			return true;
		}

		void ReleaseGns() {
			if (g_GnsRefCount == 0) {
				return;
			}
			--g_GnsRefCount;
			if (g_GnsRefCount == 0 && g_GnsInitialized) {
				const uint64_t now = GnsNowMs(), deadline = g_GnsLingerUntilMs.load();
				if (now < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(deadline - now));
				GameNetworkingSockets_Kill();
				g_GnsInitialized = false;
			}
		}

		NetTransportLane LaneFromMessage(const SteamNetworkingMessage_t& message) {
			return (message.m_nFlags & k_nSteamNetworkingSend_Reliable) != 0 ? NetTransportLane::ControlReliable : NetTransportLane::InputUnreliable;
		}

		int SendFlags(NetTransportLane lane) {
			switch (lane) {
				case NetTransportLane::ControlReliable:
				case NetTransportLane::DiagnosticsReliable:
					return k_nSteamNetworkingSend_Reliable;
				case NetTransportLane::InputUnreliable:
					return k_nSteamNetworkingSend_UnreliableNoDelay;
				case NetTransportLane::BulkUnreliable:
					return k_nSteamNetworkingSend_Unreliable;
			}
			return k_nSteamNetworkingSend_Reliable;
		}

		std::string EndDebugText(const SteamNetConnectionStatusChangedCallback_t& info) {
			return info.m_info.m_szEndDebug[0] != '\0' ? info.m_info.m_szEndDebug : "GNS connection closed";
		}

		int s_SimulatedLagMs = 0;
		int s_SimulatedJitterMs = 0;
		float s_SimulatedReorderPercent = 0;
		float s_SimulatedDuplicatePercent = 0;
		int s_RendezvousLogLevel = 0;
		static constexpr size_t c_MaxHeldBytesBeforeAnnounce = 256 * 1024;

		std::string HeldPacketOverflowReason(size_t bytes) {
			return "too much data before the connection was announced: " + std::to_string(bytes) + " > " + std::to_string(c_MaxHeldBytesBeforeAnnounce);
		}

#ifdef CCCP_WITH_GNS
		bool QueueHeldPacket(std::map<HSteamNetConnection, std::vector<NetTransportEvent>>& heldPackets,
		                    HSteamNetConnection connection, NetTransportEvent&& received) {
			size_t bytes = received.bytes.size();
			if (const auto found = heldPackets.find(connection); found != heldPackets.end())
				for (const NetTransportEvent& event : found->second) bytes += event.bytes.size();
			if (bytes > c_MaxHeldBytesBeforeAnnounce) return false;
			heldPackets[connection].push_back(std::move(received));
			return true;
		}

		void ReleaseHeldPackets(std::map<HSteamNetConnection, std::vector<NetTransportEvent>>& heldPackets,
		                      std::vector<NetTransportEvent>& pending, HSteamNetConnection connection, NetPeerId peerId) {
			pending.push_back({NetTransportEventType::PeerConnected, peerId, NetTransportLane::ControlReliable, {}, {}});
			const auto held = heldPackets.find(connection);
			if (held == heldPackets.end()) return;
			for (NetTransportEvent& event : held->second) pending.push_back(std::move(event));
			heldPackets.erase(held);
		}
#endif

		// GNS calls this on its service thread while holding its lock: print, nothing else.
		void GnsDebugOutput(ESteamNetworkingSocketsDebugOutputType type, const char* message) {
			static std::mutex mutex;
			std::istringstream lines(message ? message : "");
			std::lock_guard<std::mutex> lock(mutex);
			for (std::string line; std::getline(lines, line);) {
				if (!line.empty()) {
					System::PrintDiagnosticLine("[net-gns] " + std::to_string(static_cast<int>(type)) + " " + NetRelayLogins::Scrub(line));
				}
			}
		}

		// Diagnostics: routes GNS's own rendezvous spew into the run's log.
		void ApplyRendezvousLog() {
			if (s_RendezvousLogLevel <= 0) {
				return;
			}
			SteamNetworkingUtils()->SetDebugOutputFunction(static_cast<ESteamNetworkingSocketsDebugOutputType>(s_RendezvousLogLevel), GnsDebugOutput);
			SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, s_RendezvousLogLevel);
		}

		// Test harness: splits the requested RTT across the send/recv legs of every connection, and adds its jitter.
		void ApplySimulatedLag() {
			if (s_SimulatedLagMs > 0) {
				SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, s_SimulatedLagMs / 2);
				SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, s_SimulatedLagMs - s_SimulatedLagMs / 2);
			}
			if (s_SimulatedJitterMs > 0) {
				// The mapping a cross fault's jitter_ms gets: half on average, the whole at worst, on every packet.
				const float jitter = static_cast<float>(s_SimulatedJitterMs);
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, jitter / 2);
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Avg, jitter / 2);
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, jitter);
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Max, jitter);
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, 100.0F);
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Pct, 100.0F);
				// A jittered link's packets carry their send spacing, which plain UDP leaves out, so the receiving end's own
				// latency-variance histogram measures the jitter; it costs two bytes a packet, so no other run sends it.
				SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendTimeSincePreviousPacket, 1);
			}
			if (s_SimulatedReorderPercent > 0) {
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Send, s_SimulatedReorderPercent);
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Recv, s_SimulatedReorderPercent);
				SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, 20);
			}
			if (s_SimulatedDuplicatePercent > 0) {
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Send, s_SimulatedDuplicatePercent);
				SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Recv, s_SimulatedDuplicatePercent);
				SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketDup_TimeMax, 20);
			}
		}
	}

	struct GnsTransport::Impl {
		~Impl() {
			Stop();
			Release();
		}

		bool StartHost(uint16_t port, std::string* error) {
			Stop();
			if (!Acquire(error)) {
				return false;
			}
			if (port == 0) {
				SetError(error, "GNS host port must be nonzero");
				return false;
			}

			SteamNetworkingIPAddr listenAddress;
			listenAddress.Clear();
			listenAddress.m_port = port;

			ApplySimulatedLag();
			SteamNetworkingConfigValue_t connectionConfigs[5];
			connectionConfigs[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(SteamNetConnectionStatusChangedCallback));
			// Bulk match-state transfers (a few MB) must fit the reliable send buffer outright and
			// move faster than the conservative default send rate (~256KB/s would take seconds).
			connectionConfigs[1].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, 8 * 1024 * 1024);
			connectionConfigs[2].SetInt32(k_ESteamNetworkingConfig_SendRateMin, 2 * 1024 * 1024);
			connectionConfigs[3].SetInt32(k_ESteamNetworkingConfig_SendRateMax, 32 * 1024 * 1024);
			// A crashed peer should stall the match seconds, not the ~10s default, before the drop
			// adjudication (and a rejoiner's freed slot) kick in.
			connectionConfigs[4].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, c_NetLinkTimeoutMs);

			m_Interface = SteamNetworkingSockets();
			m_ListenSocket = m_Interface->CreateListenSocketIP(listenAddress, 5, connectionConfigs);
			if (m_ListenSocket == k_HSteamListenSocket_Invalid) {
				SetError(error, "CreateListenSocketIP failed");
				return false;
			}
			s_ListenerOwners[m_ListenSocket] = this;

			m_PollGroup = m_Interface->CreatePollGroup();
			if (m_PollGroup == k_HSteamNetPollGroup_Invalid) {
				SetError(error, "CreatePollGroup failed");
				Stop();
				return false;
			}

			m_IsHost = true;
			m_IsStarted = true;
			m_NextPeerId = 1;
			return true;
		}

		bool Connect(const std::string& address, uint16_t port, std::string* error) {
			Stop();
			if (!Acquire(error)) {
				return false;
			}
			if (address.empty()) {
				SetError(error, "GNS connect address must be non-empty");
				return false;
			}
			if (port == 0) {
				SetError(error, "GNS connect port must be nonzero");
				return false;
			}

			std::string endpoint = address;
			if (endpoint.find(':') == std::string::npos) {
				endpoint += ":" + std::to_string(port);
			}

			SteamNetworkingIPAddr remoteAddress;
			remoteAddress.Clear();
			if (!remoteAddress.ParseString(endpoint.c_str())) {
				SetError(error, "GNS could not parse address '" + endpoint + "'");
				return false;
			}
			if (remoteAddress.m_port == 0) {
				remoteAddress.m_port = port;
			}

			ApplySimulatedLag();
			SteamNetworkingConfigValue_t connectionConfigs[5];
			connectionConfigs[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(SteamNetConnectionStatusChangedCallback));
			// Bulk match-state transfers (a few MB) must fit the reliable send buffer outright and
			// move faster than the conservative default send rate (~256KB/s would take seconds).
			connectionConfigs[1].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, 8 * 1024 * 1024);
			connectionConfigs[2].SetInt32(k_ESteamNetworkingConfig_SendRateMin, 2 * 1024 * 1024);
			connectionConfigs[3].SetInt32(k_ESteamNetworkingConfig_SendRateMax, 32 * 1024 * 1024);
			// A crashed peer should stall the match seconds, not the ~10s default, before the drop
			// adjudication (and a rejoiner's freed slot) kick in.
			connectionConfigs[4].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, c_NetLinkTimeoutMs);

			m_Interface = SteamNetworkingSockets();
			m_ServerConnection = m_Interface->ConnectByIPAddress(remoteAddress, 5, connectionConfigs);
			if (m_ServerConnection == k_HSteamNetConnection_Invalid) {
				SetError(error, "ConnectByIPAddress failed");
				return false;
			}

			m_IsHost = false;
			m_IsStarted = true;
			m_PeersByConnection[m_ServerConnection] = 1;
			m_ConnectionsByPeer[1] = m_ServerConnection;
			s_ConnectionOwners[m_ServerConnection] = this;
			return true;
		}

		bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) {
			if (congested) *congested = false;
			if (!m_IsStarted || !m_Interface) {
				SetError(error, "GNS transport is not started");
				return false;
			}
			if (bytes.size() > std::numeric_limits<uint32_t>::max()) {
				SetError(error, "GNS packet is too large");
				return false;
			}

			const auto connectionIt = m_ConnectionsByPeer.find(peerId);
			if (connectionIt == m_ConnectionsByPeer.end()) {
				SetError(error, "GNS peer was not found");
				return false;
			}

			if (!RouteAllowed(connectionIt->second)) {
				SetError(error, "ICE route refused by the player's Connection setting");
				return false;
			}
			s_TestUplinkStall.Update(UplinkStallClockMs());
			const EResult result = m_Interface->SendMessageToConnection(
				connectionIt->second,
				bytes.data(),
				static_cast<uint32>(bytes.size()),
				SendFlags(lane),
				nullptr);
			if (result != k_EResultOK) {
				// LimitExceeded means the queue is full, not that the peer is gone: the message was
				// never taken, so the caller holds it rather than giving up on a reachable player.
				if (congested) *congested = result == k_EResultLimitExceeded;
				SetError(error, "SendMessageToConnection failed with EResult " + std::to_string(static_cast<int>(result)) +
				                " (" + std::to_string(bytes.size()) + " bytes, handed over " + std::to_string(m_BytesHandedOver[peerId]) +
				                DescribeSendPressure(connectionIt->second) + ")");
				return false;
			}
			m_BytesHandedOver[peerId] += bytes.size();
			TraceLobbyDelivery(connectionIt->second, bytes, "sent");
			return true;
		}

		void TraceLobbyDelivery(HSteamNetConnection connection, const std::vector<uint8_t>& bytes, const char* direction) {
			// The header keeps frame sends out of the lobby decoder.
			if (bytes.size() < NetLobbyProtocol::c_HeaderBytes || bytes.size() > NetLobbyProtocol::c_MaxStateChunkBytes + 128) return;
			uint32_t magic = 0;
			for (size_t i = 0; i < 4; ++i) magic |= static_cast<uint32_t>(bytes[i]) << (8 * i);
			if (magic != NetLobbyProtocol::c_Magic) return;
			const auto decoded = NetLobbyProtocol::Decode(bytes);
			if (!decoded.ok || (!std::holds_alternative<NetLobbyStart>(decoded.message.payload) &&
			    !std::holds_alternative<NetLobbyStateChunk>(decoded.message.payload) &&
			    !(std::holds_alternative<NetLobbyConfigAck>(decoded.message.payload) && std::get<NetLobbyConfigAck>(decoded.message.payload).reason.starts_with("state:")))) return;
			const auto nowUs = SteamNetworkingUtils()->GetLocalTimestamp();
			auto& lastUs = m_LastDeliveryTraceUs[connection];
			if (lastUs != 0 && nowUs - lastUs < 1000000) return;
			lastUs = nowUs;
			SteamNetConnectionRealTimeStatus_t status{};
			if (m_Interface->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) != k_EResultOK) return;
			DiagnosticLine() << "[net-start-delivery] " << direction << " type=" << NetLobbyProtocol::MessageTypeName(NetLobbyProtocol::MessageTypeOf(decoded.message.payload))
			                 << " bytes=" << bytes.size() << " connection=" << connection << " clock_ms=" << nowUs / 1000
			                 << " pending_reliable=" << status.m_cbPendingReliable << " unacked_reliable=" << status.m_cbSentUnackedReliable
			                 << " queue_ms=" << status.m_usecQueueTime / 1000 << " ping_ms=" << status.m_nPing
			                 << " send_rate=" << status.m_nSendRateBytesPerSecond << " local_quality=" << status.m_flConnectionQualityLocal
			                 << " remote_quality=" << status.m_flConnectionQualityRemote << std::endl;
		}

		// What the connection was holding when it refused. k_EResultLimitExceeded (25) means the
		// pending bytes reached SendBufferSize, so the refusal is only readable next to that budget
		// and the rate draining it.
		std::string DescribeSendPressure(HSteamNetConnection connection) {
			std::string text;
			SteamNetConnectionRealTimeStatus_t status{};
			if (m_Interface->GetConnectionRealTimeStatus(connection, &status, 0, nullptr) == k_EResultOK) {
				text += ", pending reliable " + std::to_string(status.m_cbPendingReliable) +
				        ", unreliable " + std::to_string(status.m_cbPendingUnreliable) +
				        ", unacked " + std::to_string(status.m_cbSentUnackedReliable) +
				        ", rate " + std::to_string(status.m_nSendRateBytesPerSecond) + " B/s" +
				        ", queue " + std::to_string(status.m_usecQueueTime / 1000) + "ms" +
				        ", ping " + std::to_string(status.m_nPing) + "ms";
			}
			int32 budget = 0;
			size_t budgetSize = sizeof(budget);
			ESteamNetworkingConfigDataType type = k_ESteamNetworkingConfig_Int32;
			if (SteamNetworkingUtils()->GetConfigValue(k_ESteamNetworkingConfig_SendBufferSize, k_ESteamNetworkingConfig_Connection,
			                                           connection, &type, &budget, &budgetSize) >= k_ESteamNetworkingGetConfigValue_OK) {
				text += ", buffer budget " + std::to_string(budget);
			}
			// The pending figure counts data scheduled for RE-transmission as well as new data, so it
			// only means something beside what we actually handed over and the loss that drove it. A
			// saturated episode refuses thousands of times a second, so take the 2KB dump once a second.
			constexpr SteamNetworkingMicroseconds c_DetailIntervalUs = 1000 * 1000;
			const SteamNetworkingMicroseconds nowUs = SteamNetworkingUtils()->GetLocalTimestamp();
			SteamNetworkingMicroseconds& lastUs = m_LastDetailUs[connection];
			if (lastUs != 0 && nowUs - lastUs < c_DetailIntervalUs) {
				return text;
			}
			lastUs = nowUs;
			char detail[2048] = {};
			if (m_Interface->GetDetailedConnectionStatus(connection, detail, sizeof(detail)) == 0) {
				std::string status(detail);
				std::replace(status.begin(), status.end(), '\n', ' ');
				text += ", detail: " + status;
			}
			return text;
		}

		void Disconnect(NetPeerId peerId, const std::string& reason) {
			const auto connectionIt = m_ConnectionsByPeer.find(peerId);
			if (!m_Interface || connectionIt == m_ConnectionsByPeer.end()) {
				return;
			}
			const HSteamNetConnection connection = connectionIt->second;
			// Bypass the Nagle timer so queued reliable data (e.g. a join-reject) beats the close onto the wire.
			m_Interface->FlushMessagesOnConnection(connection);
			m_Interface->CloseConnection(connection, 0, reason.c_str(), true);
			KeepGnsForLingeringClose();
			ForgetConnection(connection);
			// GNS reports nothing for a close we made ourselves, and forgetting the handle means its own
			// later callback finds no peer either. A peer leaving must look the same to us however it
			// went, or state keyed on the connection - a held seat, most of all - is never cleaned up.
			DiagnosticLine() << "[net-transport] closed peer=" << peerId << " reason=" << reason << std::endl;
			m_PendingEvents.push_back({NetTransportEventType::PeerDisconnected, peerId, NetTransportLane::ControlReliable, {}, reason});
		}

		void Stop() {
			m_RouteLogged.clear(); m_CandidateIdentities.clear(); m_CandidateTypes.clear();
			m_ConnectionOffers.clear(); m_RouteReceipts.clear(); m_RelayOffer = "none";
			m_RouteTracker.Clear(); m_DialedMs.clear();
			m_Announced.clear(); m_HeldPackets.clear();
			m_P2PMode = -1;
			if (!m_Interface) {
				m_IsHost = false;
				m_IsStarted = false;
				m_PendingEvents.clear();
				return;
			}

			std::vector<HSteamNetConnection> connections;
			connections.reserve(m_PeersByConnection.size());
			for (const auto& [connection, peerId] : m_PeersByConnection) {
				(void)peerId;
				connections.push_back(connection);
			}
			for (HSteamNetConnection connection : connections) {
				// Flush + linger so a queued goodbye (lockstep stop, session close) reaches the peer.
				m_Interface->FlushMessagesOnConnection(connection);
				m_Interface->CloseConnection(connection, 0, "transport stopped", true);
				KeepGnsForLingeringClose();
				ForgetConnection(connection);
			}

			if (m_ListenSocket != k_HSteamListenSocket_Invalid) {
				s_ListenerOwners.erase(m_ListenSocket);
				m_Interface->CloseListenSocket(m_ListenSocket);
				m_ListenSocket = k_HSteamListenSocket_Invalid;
			}
			if (m_PollGroup != k_HSteamNetPollGroup_Invalid) {
				m_Interface->DestroyPollGroup(m_PollGroup);
				m_PollGroup = k_HSteamNetPollGroup_Invalid;
			}

			m_ServerConnection = k_HSteamNetConnection_Invalid;
			m_Interface = nullptr;
			m_IsHost = false;
			m_IsStarted = false;
			m_NextPeerId = 1;
			m_BytesHandedOver.clear();
			m_LastDetailUs.clear();
			m_LastDeliveryTraceUs.clear();
			m_LiveTurnLogin.clear();
			m_PendingEvents.clear();
		}

		std::vector<NetTransportEvent> PollEvents() {
			if (m_Interface) {
				s_TestUplinkStall.Update(UplinkStallClockMs());
				// Drain delivered messages first: a close callback forgets the connection, which would
				// drop a reject/goodbye that GNS already delivered alongside it.
				PollIncomingMessages();
				if (m_P2PMode >= 0) {
					PollCallbacks();
					NoteRouteChanges();
					const auto connections = m_PeersByConnection;
					for (const auto& [connection, peer] : connections) {
						if (!RouteAllowed(connection)) RefuseRoute(connection);
					}
					PollIncomingMessages();
				}
				PollCallbacks();
			}

			std::vector<NetTransportEvent> events;
			events.swap(m_PendingEvents);
			return events;
		}

		uint32_t GetPeerPingMs(NetPeerId peerId) {
			const auto connectionIt = m_ConnectionsByPeer.find(peerId);
			if (!m_Interface || connectionIt == m_ConnectionsByPeer.end()) {
				return 0;
			}
			SteamNetConnectionRealTimeStatus_t status{};
			if (m_Interface->GetConnectionRealTimeStatus(connectionIt->second, &status, 0, nullptr) != k_EResultOK) {
				return 0;
			}
			return status.m_nPing > 0 ? static_cast<uint32_t>(status.m_nPing) : 0;
		}

		bool IsPeerPingMeasured(NetPeerId peerId) {
			const auto connectionIt = m_ConnectionsByPeer.find(peerId);
			if (!m_Interface || connectionIt == m_ConnectionsByPeer.end()) {
				return false;
			}
			SteamNetConnectionRealTimeStatus_t status{};
			// The library reads a link it has no round trip for yet as a negative ping.
			return m_Interface->GetConnectionRealTimeStatus(connectionIt->second, &status, 0, nullptr) == k_EResultOK && status.m_nPing >= 0;
		}

		bool Acquire(std::string* error) {
			if (m_HasGnsRef) {
				return true;
			}
			if (!AcquireGns(error)) {
				return false;
			}
			m_HasGnsRef = true;
			return true;
		}

		void Release() {
			if (m_HasGnsRef) {
				ReleaseGns();
				m_HasGnsRef = false;
			}
		}

		void PollCallbacks() {
			const uint64_t reset = s_CrossTransportResetMs.load();
			const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			if (reset && now >= reset && s_CrossTransportResetMs.exchange(0)) {
				const bool accepted = ApplyCrossTransportFault(0, 0, 0, 0);
				System::PrintDiagnosticLine("[cross-transport] timed_reset send_recv_armed=" + std::to_string(accepted));
			}
			m_Interface->RunCallbacks();
		}

		void PollIncomingMessages() {
			// A client whose connection closed has nothing left to receive; asking for it is not a broken pump.
			if (!m_IsHost && m_ServerConnection == k_HSteamNetConnection_Invalid) return;
			while (m_Interface && m_IsStarted) {
				SteamNetworkingMessage_t* message = nullptr;
				const int count = m_IsHost
					? m_Interface->ReceiveMessagesOnPollGroup(m_PollGroup, &message, 1)
					: m_Interface->ReceiveMessagesOnConnection(m_ServerConnection, &message, 1);
				if (count == 0) {
					break;
				}
				if (count < 0) {
					// Our own receive pump is broken - the match genuinely cannot continue. Distinct
					// from a joiner's connection faulting, which must never stop a running match.
					m_PendingEvents.push_back({NetTransportEventType::LocalTransportFault, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "GNS ReceiveMessages failed"});
					break;
				}
				if (!message) {
					break;
				}

				const auto peerIt = m_PeersByConnection.find(message->m_conn);
				if (peerIt != m_PeersByConnection.end() && RouteAllowed(message->m_conn)) {
					const uint8_t* data = static_cast<const uint8_t*>(message->m_pData);
					NetTransportEvent received{
						NetTransportEventType::PacketReceived,
						peerIt->second,
						LaneFromMessage(*message),
						std::vector<uint8_t>(data, data + message->m_cbSize),
						{}};
					TraceLobbyDelivery(message->m_conn, received.bytes, "received");
					// GNS decrypts a P2P peer's payload as soon as the rendezvous is done, which over a
					// relay routinely beats our own Connected callback. Handing it up before the session
					// has been told the peer exists loses it, so it waits behind that announcement.
					if (m_Announced.contains(message->m_conn)) {
						m_PendingEvents.push_back(std::move(received));
					} else {
						HoldUntilAnnounced(message->m_conn, std::move(received));
					}
				}
				message->Release();
			}
		}

		std::string CandidateType(const SteamNetConnectionInfo_t& info) const {
			if ((info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0) return "relay";
			if (m_P2PMode < 0 || (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers) != 0) return "host";
			char identity[SteamNetworkingIdentity::k_cchMaxString]{}, address[SteamNetworkingIPAddr::k_cchMaxString]{};
			info.m_identityRemote.ToString(identity, sizeof(identity)); info.m_addrRemote.ToString(address, sizeof(address), true);
			const auto candidate = m_CandidateTypes.find({identity, address});
			return candidate == m_CandidateTypes.end() ? "prflx" : candidate->second;
		}

		mutable std::set<HSteamNetConnection> m_RouteLogged;
		mutable std::map<HSteamNetConnection, std::string> m_RouteReceipts; //!< Each connection's latest [net-route] line.
		mutable GnsRouteTracker m_RouteTracker; //!< The route each receipt named, so a live change gets its own receipt.
		std::map<HSteamNetConnection, uint64_t> m_DialedMs; //!< When each ICE connection was dialed or accepted.
		std::map<HSteamNetConnection, std::string> m_ConnectionOffers; //!< The relay offer each connection's TURN lists came from.
		std::string m_RelayOffer = "none"; //!< The offer a connection made or accepted now runs with.
		std::map<uint32_t, std::string> m_CandidateIdentities;
		std::map<std::pair<std::string, std::string>, std::string> m_CandidateTypes;

		bool RouteAllowed(HSteamNetConnection connection) const {
			if (m_P2PMode <= 0 && m_RouteLogged.contains(connection)) return true;
			SteamNetConnectionInfo_t info{};
			if (!m_Interface->GetConnectionInfo(connection, &info) || info.m_eState != k_ESteamNetworkingConnectionState_Connected) return true;
			const bool relayed = (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0;
			const bool allowed = GnsTransport::ConnectionPolicyAllowsRoute(m_P2PMode, relayed);
			if (m_RouteLogged.insert(connection).second) {
				m_RouteTracker.Observe(connection, relayed);
				if (const auto dialed = m_DialedMs.find(connection); dialed != m_DialedMs.end()) {
					DiagnosticLine() << "[net-ice] connected connection=" << connection << " after_ms=" << SteadyMs() - dialed->second << " route=" << (relayed ? "relay" : "direct") << std::endl;
				}
				WriteRouteReceipt(connection, info, relayed, allowed, nullptr);
			}
			return allowed;
		}

		static uint64_t SteadyMs() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		// Digests bind the relay to its offer without writing the player's addresses.
		void WriteRouteReceipt(HSteamNetConnection connection, const SteamNetConnectionInfo_t& info, bool relayed, bool allowed, const char* change) const {
			DiagnosticLine() << "[net-ice] selected candidate=" << CandidateType(info) << " connection=" << connection << std::endl;
			char address[SteamNetworkingIPAddr::k_cchMaxString]{};
			info.m_addrRemote.ToString(address, sizeof(address), true);
			std::ostringstream line;
			line << "[net-route] RouteAllowed route=" << (relayed ? "relay" : "direct") << " allowed=" << (allowed ? 1 : 0) << " connection=" << connection
			     << " remote_sha256=" << (info.m_addrRemote.IsIPv6AllZeros() ? std::string("none") : System::Sha256Hex(address, std::char_traits<char>::length(address)));
			if (relayed) line << " turn_sha256=" << TurnHostReceipts(ConnectionConfigString(connection, k_ESteamNetworkingConfig_P2P_TURN_ServerList));
			line << " offer=" << RouteOffer(connection, relayed);
			if (change) {
				line << " change=" << change;
				if (const auto dialed = m_DialedMs.find(connection); dialed != m_DialedMs.end()) line << " after_ms=" << SteadyMs() - dialed->second;
			}
			m_RouteReceipts[connection] = line.str();
			DiagnosticLine() << line.str() << std::endl;
		}

		// ICE keeps testing candidate pairs after the first route, so a live connection can move between relay and direct.
		void NoteRouteChanges() {
			for (const auto& [connection, peer] : m_PeersByConnection) {
				(void)peer;
				if (!m_RouteLogged.contains(connection)) continue;
				SteamNetConnectionInfo_t info{};
				if (!m_Interface->GetConnectionInfo(connection, &info) || info.m_eState != k_ESteamNetworkingConnectionState_Connected) continue;
				const bool relayed = (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0;
				if (m_RouteTracker.Observe(connection, relayed) == GnsRouteTracker::Observation::Moved) {
					WriteRouteReceipt(connection, info, relayed, GnsTransport::ConnectionPolicyAllowsRoute(m_P2PMode, relayed), GnsRouteTracker::MoveName(relayed));
				}
			}
		}

		// A direct route uses no relay offer, whatever login its connection holds.
		std::string RouteOffer(HSteamNetConnection connection, bool relayed) const {
			const auto offer = m_ConnectionOffers.find(connection);
			return relayed && offer != m_ConnectionOffers.end() ? offer->second : std::string("none");
		}

		void RefuseRoute(HSteamNetConnection connection) {
			const char* localReason = m_P2PMode == 1
			    ? "Your Connection is Direct only. Switch it to Automatic or Relay only to use this relay route."
			    : "Your Connection is Relay only, but this route is direct. Check your relay or switch Connection to Automatic.";
			const char* remoteReason = m_P2PMode == 1
			    ? (m_IsHost ? "The host's Connection is Direct only. Ask the host to switch it to Automatic to allow this relay route."
			                : "The joining player's Connection is Direct only. Ask that player to switch it to Automatic to allow this relay route.")
			    : (m_IsHost ? "The host's Connection is Relay only, but this route is direct. Ask the host to check the relay, then retry."
			                : "The joining player's Connection is Relay only, but this route is direct. Ask that player to check the relay, then retry.");
			m_Interface->CloseConnection(connection, 0, remoteReason, false);
			HandleConnectionClosed(connection, localReason, k_ESteamNetworkingConnectionState_Connecting);
		}

		void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
			if (!info || !m_Interface) {
				return;
			}

			switch (info->m_info.m_eState) {
				case k_ESteamNetworkingConnectionState_Connecting:
					if (m_IsHost && info->m_info.m_hListenSocket == m_ListenSocket) {
						s_ConnectionOwners[info->m_hConn] = this;
						AcceptIncomingConnection(info->m_hConn);
					}
					break;
				case k_ESteamNetworkingConnectionState_Connected:
					if (!RouteAllowed(info->m_hConn)) {
						RefuseRoute(info->m_hConn);
						break;
					}
					if (m_IsHost && m_P2PMode >= 0) {
						const auto peer = m_PeersByConnection.find(info->m_hConn);
						if (peer != m_PeersByConnection.end()) AnnounceConnected(info->m_hConn, peer->second);
					}
					if (!m_IsHost && info->m_hConn == m_ServerConnection) {
						EnsureClientConnected(info->m_hConn);
					}
					break;
				case k_ESteamNetworkingConnectionState_ClosedByPeer:
				case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
					const std::string reason = EndDebugText(*info);
					// A terminal callback still owns a native connection until the
					// application closes it. Retiring only our maps leaks its ICE
					// requests and relay allocations into every later retry.
					m_Interface->CloseConnection(info->m_hConn, 0, nullptr, false);
					HandleConnectionClosed(info->m_hConn, reason, info->m_eOldState);
					break;
				}
				case k_ESteamNetworkingConnectionState_None:
				default:
					break;
			}
		}

		void HoldUntilAnnounced(HSteamNetConnection connection, NetTransportEvent&& received) {
			size_t bytes = received.bytes.size();
			if (const auto found = m_HeldPackets.find(connection); found != m_HeldPackets.end())
				for (const NetTransportEvent& event : found->second) bytes += event.bytes.size();
			if (!QueueHeldPacket(m_HeldPackets, connection, std::move(received))) {
				const std::string reason = HeldPacketOverflowReason(bytes);
				m_Interface->CloseConnection(connection, 0, reason.c_str(), false);
				HandleConnectionClosed(connection, reason, k_ESteamNetworkingConnectionState_Connecting);
				return;
			}
		}

		void AnnounceConnected(HSteamNetConnection connection, NetPeerId peerId) {
			m_Announced.insert(connection);
			ReleaseHeldPackets(m_HeldPackets, m_PendingEvents, connection, peerId);
		}

		bool PayloadHoldSelfTest(std::string* error) {
			m_Announced.clear();
			m_HeldPackets.clear();
			m_PendingEvents.clear();
			const HSteamNetConnection connection = static_cast<HSteamNetConnection>(41);
			HoldUntilAnnounced(connection, NetTransportEvent{NetTransportEventType::PacketReceived, 2, NetTransportLane::ControlReliable, {1, 2, 3}, {}});
			HoldUntilAnnounced(connection, NetTransportEvent{NetTransportEventType::PacketReceived, 2, NetTransportLane::InputUnreliable, {4, 5}, {}});
			AnnounceConnected(connection, 2);
			if (m_PendingEvents.size() != 3 || m_PendingEvents[0].type != NetTransportEventType::PeerConnected ||
			    m_PendingEvents[1].bytes != std::vector<uint8_t>({1, 2, 3}) || m_PendingEvents[2].bytes != std::vector<uint8_t>({4, 5})) {
				if (error) *error = "the announced transport did not release held packets in order: events=" + std::to_string(m_PendingEvents.size()) +
				                  " first_type=" + std::to_string(static_cast<int>(m_PendingEvents.empty() ? NetTransportEventType::TransportError : m_PendingEvents[0].type));
				return false;
			}
			return true;
		}

		void AcceptIncomingConnection(HSteamNetConnection connection) {
			m_ConnectionOffers[connection] = m_RelayOffer;
			if (m_P2PMode >= 0) m_DialedMs[connection] = SteadyMs();
			if (m_Interface->AcceptConnection(connection) != k_EResultOK) {
				m_Interface->CloseConnection(connection, 0, "accept failed", false);
				m_PendingEvents.push_back({NetTransportEventType::TransportError, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "GNS AcceptConnection failed"});
				return;
			}
			if (m_PollGroup != k_HSteamNetPollGroup_Invalid && !m_Interface->SetConnectionPollGroup(connection, m_PollGroup)) {
				m_Interface->CloseConnection(connection, 0, "poll group assignment failed", false);
				m_PendingEvents.push_back({NetTransportEventType::TransportError, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "GNS SetConnectionPollGroup failed"});
				return;
			}

			const NetPeerId peerId = m_NextPeerId++;
			m_PeersByConnection[connection] = peerId;
			m_ConnectionsByPeer[peerId] = connection;
			s_ConnectionOwners[connection] = this;
			if (m_P2PMode < 0) AnnounceConnected(connection, peerId);
		}

		void EnsureClientConnected(HSteamNetConnection connection) {
			if (m_PeersByConnection.find(connection) == m_PeersByConnection.end()) {
				m_PeersByConnection[connection] = 1;
				m_ConnectionsByPeer[1] = connection;
				s_ConnectionOwners[connection] = this;
			}
			AnnounceConnected(connection, 1);
		}

		void HandleConnectionClosed(HSteamNetConnection connection, const std::string& reason, ESteamNetworkingConnectionState oldState) {
			const auto peerIt = m_PeersByConnection.find(connection);
			if (peerIt == m_PeersByConnection.end()) {
				if (oldState == k_ESteamNetworkingConnectionState_Connecting) {
					m_PendingEvents.push_back({NetTransportEventType::ConnectionFailed, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, reason});
				}
				if (m_Interface) {
					m_Interface->CloseConnection(connection, 0, nullptr, false);
				}
				s_ConnectionOwners.erase(connection);
				return;
			}

			const NetPeerId peerId = peerIt->second;
			if (const auto dialed = m_DialedMs.find(connection); dialed != m_DialedMs.end() && !m_RouteLogged.contains(connection)) {
				DiagnosticLine() << "[net-ice] connect ended connection=" << connection << " after_ms=" << SteadyMs() - dialed->second
				                 << " limit_ms=" << GnsTransport::IceConnectTimeoutMs() << std::endl;
			}
			ForgetConnection(connection);
			DiagnosticLine() << "[net-transport] closed peer=" << peerId << " reason=" << reason << std::endl;
			m_PendingEvents.push_back({NetTransportEventType::PeerDisconnected, peerId, NetTransportLane::ControlReliable, {}, reason});
		}

		void ForgetConnection(HSteamNetConnection connection) {
			m_RouteLogged.erase(connection);
			m_RouteTracker.Forget(connection);
			m_DialedMs.erase(connection);
			m_ConnectionOffers.erase(connection);
			m_RouteReceipts.erase(connection);
			m_Announced.erase(connection);
			m_HeldPackets.erase(connection);
			const auto peerIt = m_PeersByConnection.find(connection);
			if (peerIt != m_PeersByConnection.end()) {
				m_ConnectionsByPeer.erase(peerIt->second);
				m_BytesHandedOver.erase(peerIt->second);
				m_PeersByConnection.erase(peerIt);
			}
			s_ConnectionOwners.erase(connection);
			m_LastDetailUs.erase(connection);
			m_LastDeliveryTraceUs.erase(connection);
			if (connection == m_ServerConnection) {
				m_ServerConnection = k_HSteamNetConnection_Invalid;
			}
		}

		static void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* info) {
			if (!info) {
				return;
			}
			const auto ownerIt = s_ConnectionOwners.find(info->m_hConn);
			if (ownerIt != s_ConnectionOwners.end() && ownerIt->second) {
				ownerIt->second->OnConnectionStatusChanged(info);
				return;
			}
			// A process-wide callback belongs to its listener, not the transport polling it.
			if (info->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
				const auto listener = s_ListenerOwners.find(info->m_info.m_hListenSocket);
				if (listener != s_ListenerOwners.end()) listener->second->OnConnectionStatusChanged(info);
			}
		}

		bool StartHostP2P(int virtualPort, const GnsP2PConfig& config, std::string* error) {
			Stop();
			m_P2PMode = config.connectionMode;
			if (!Acquire(error)) {
				return false;
			}
			if (virtualPort < 0 || virtualPort > 0xffff) {
				SetError(error, "GNS P2P virtual port must be 0-65535");
				return false;
			}

			m_Interface = SteamNetworkingSockets();
			if (!ApplyP2PIdentity(config, error)) {
				return false;
			}
			ApplySimulatedLag();
			ApplyRendezvousLog();
			std::vector<SteamNetworkingConfigValue_t> connectionConfigs = P2PConnectionConfigs(config);
			m_ListenSocket = m_Interface->CreateListenSocketP2P(virtualPort, static_cast<int>(connectionConfigs.size()), connectionConfigs.data());
			if (m_ListenSocket == k_HSteamListenSocket_Invalid) {
				SetError(error, "CreateListenSocketP2P failed");
				return false;
			}
			s_ListenerOwners[m_ListenSocket] = this;

			m_PollGroup = m_Interface->CreatePollGroup();
			if (m_PollGroup == k_HSteamNetPollGroup_Invalid) {
				SetError(error, "CreatePollGroup failed");
				Stop();
				return false;
			}

			m_IsHost = true;
			m_IsStarted = true;
			m_NextPeerId = 1;
			m_LiveTurnLogin = config.turnServerList + '\n' + config.turnUserList + '\n' + config.turnPassList;
			m_RelayOffer = config.relayOffer;
			return true;
		}

		bool ConnectP2P(ISteamNetworkingConnectionSignaling* signaling, const std::string& peerIdentity, int remoteVirtualPort, const GnsP2PConfig& config, std::string* error) {
			Stop();
			m_P2PMode = config.connectionMode;
			if (!signaling) {
				SetError(error, "GNS P2P connect needs a signaling object");
				return false;
			}

			SteamNetworkingIdentity peer;
			peer.Clear();
			if (remoteVirtualPort < 0 || remoteVirtualPort > 0xffff) {
				SetError(error, "GNS P2P virtual port must be 0-65535");
			} else if (!peerIdentity.empty() && !peer.ParseString(peerIdentity.c_str())) {
				SetError(error, "GNS could not parse peer identity '" + peerIdentity + "'");
			} else if (Acquire(error)) {
				m_Interface = SteamNetworkingSockets();
				if (ApplyP2PIdentity(config, error)) {
					ApplySimulatedLag();
					ApplyRendezvousLog();
					std::vector<SteamNetworkingConfigValue_t> connectionConfigs = P2PConnectionConfigs(config);
					if (config.localVirtualPort >= 0) {
						connectionConfigs.emplace_back();
						connectionConfigs.back().SetInt32(k_ESteamNetworkingConfig_LocalVirtualPort, config.localVirtualPort);
					}
					// From this call on GNS owns signaling, and releases it itself if the call fails.
					m_ServerConnection = m_Interface->ConnectP2PCustomSignaling(signaling, peer.IsInvalid() ? nullptr : &peer, remoteVirtualPort, static_cast<int>(connectionConfigs.size()), connectionConfigs.data());
					if (m_ServerConnection == k_HSteamNetConnection_Invalid) {
						SetError(error, "ConnectP2PCustomSignaling failed");
						return false;
					}

					m_IsHost = false;
					m_IsStarted = true;
					m_PeersByConnection[m_ServerConnection] = 1;
					m_ConnectionsByPeer[1] = m_ServerConnection;
					s_ConnectionOwners[m_ServerConnection] = this;
					m_LiveTurnLogin = config.turnServerList + '\n' + config.turnUserList + '\n' + config.turnPassList;
					m_RelayOffer = config.relayOffer;
					m_ConnectionOffers[m_ServerConnection] = config.relayOffer;
					m_DialedMs[m_ServerConnection] = SteadyMs();
					return true;
				}
			}
			signaling->Release();
			return false;
		}

		bool ReceiveP2PSignal(const void* blob, int size, ISteamNetworkingSignalingRecvContext* context) {
			if (!m_Interface || !blob || size <= 0) {
				return false;
			}
			OwningRecvContext owningContext(this, context);
			const bool accepted = m_Interface->ReceivedP2PCustomSignal(blob, size, &owningContext);
			if (accepted) {
				auto evidence = ReadCandidateEvidence({static_cast<const char*>(blob), static_cast<size_t>(size)});
				if (!evidence.identity.empty() && evidence.connection && m_CandidateIdentities.size() < 512) m_CandidateIdentities[evidence.connection] = evidence.identity;
				if (evidence.identity.empty()) if (const auto known = m_CandidateIdentities.find(evidence.connection); known != m_CandidateIdentities.end()) evidence.identity = known->second;
				if (!evidence.identity.empty()) for (const auto& [address, type]: evidence.candidates)
					if (m_CandidateTypes.size() < 512 || m_CandidateTypes.contains({evidence.identity, address})) m_CandidateTypes[{evidence.identity, address}] = type;
			}
			return accepted;
		}

		GnsPeerConnectionInfo GetPeerConnectionInfo(NetPeerId peerId) const {
			const auto connectionIt = m_ConnectionsByPeer.find(peerId);
			return connectionIt == m_ConnectionsByPeer.end() ? GnsPeerConnectionInfo{} : ConnectionInfo(connectionIt->second, peerId);
		}

		static std::vector<GnsProcessConnection> ProcessConnections() {
			std::vector<GnsProcessConnection> connections;
			for (const Impl* impl : s_LiveImpls) {
				for (const auto& [peerId, connection] : impl->m_ConnectionsByPeer) {
					connections.push_back({impl->m_Facade, impl->m_P2PMode >= 0, impl->m_IsHost, impl->ConnectionInfo(connection, peerId)});
				}
			}
			return connections;
		}

		GnsPeerConnectionInfo ConnectionInfo(HSteamNetConnection connection, NetPeerId peerId) const {
			GnsPeerConnectionInfo result;
			result.peerId = peerId;
			SteamNetConnectionInfo_t info{};
			if (!m_Interface || !m_Interface->GetConnectionInfo(connection, &info)) {
				return result;
			}
			char identity[SteamNetworkingIdentity::k_cchMaxString] = {};
			info.m_identityRemote.ToString(identity, sizeof(identity));
			char address[SteamNetworkingIPAddr::k_cchMaxString] = {};
			info.m_addrRemote.ToString(address, sizeof(address), true);

			result.found = true;
			result.state = info.m_eState;
			result.endReason = info.m_eEndReason;
			result.endDebug = info.m_szEndDebug;
			result.description = info.m_szConnectionDescription;
			result.remoteIdentity = identity;
			result.remoteAddress = info.m_addrRemote.IsIPv6AllZeros() ? std::string() : std::string(address);
			result.flags = info.m_nFlags;
			result.relayPop = info.m_idPOPRelay;
			if (info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
				result.connectedRoute = (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0 ? "relay" : "direct";
				result.selectedCandidateType = CandidateType(info);
			}
			result.relayOffer = RouteOffer(connection, (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) != 0);
			if (const auto receipt = m_RouteReceipts.find(connection); receipt != m_RouteReceipts.end()) result.routeReceipt = receipt->second;

			const std::pair<const char*, ESteamNetworkingConfigValue> numbers[] = {
				{"P2P_Transport_ICE_Enable", k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable},
				{"P2P_Transport_ICE_Implementation", k_ESteamNetworkingConfig_P2P_Transport_ICE_Implementation},
				{"LogLevel_P2PRendezvous", k_ESteamNetworkingConfig_LogLevel_P2PRendezvous},
				{"LocalVirtualPort", k_ESteamNetworkingConfig_LocalVirtualPort},
				{"SendBufferSize", k_ESteamNetworkingConfig_SendBufferSize},
				{"SendRateMin", k_ESteamNetworkingConfig_SendRateMin},
				{"SendRateMax", k_ESteamNetworkingConfig_SendRateMax},
				{"TimeoutInitial", k_ESteamNetworkingConfig_TimeoutInitial},
				{"TimeoutConnected", k_ESteamNetworkingConfig_TimeoutConnected},
			};
			for (const auto& [name, value] : numbers) {
				result.config.push_back(std::string(name) + "=" + std::to_string(ConnectionConfigInt32(connection, value)));
			}
			result.config.push_back("P2P_STUN_ServerList=\"" + ConnectionConfigString(connection, k_ESteamNetworkingConfig_P2P_STUN_ServerList) + "\"");
			return result;
		}

		std::string GetPeerDetailedStatus(NetPeerId peerId) {
			const auto connectionIt = m_ConnectionsByPeer.find(peerId);
			if (!m_Interface || connectionIt == m_ConnectionsByPeer.end()) {
				return {};
			}
			std::vector<char> detail(16 * 1024, '\0');
			if (m_Interface->GetDetailedConnectionStatus(connectionIt->second, detail.data(), static_cast<int>(detail.size())) != 0) {
				return {};
			}
			return detail.data();
		}

		NetFakeLinkEffects GetFakeLinkEffects() {
			NetFakeLinkEffects total;
			if (!m_Interface) return total;
			std::vector<char> detail(16 * 1024, '\0');
			for (const auto& [connection, peerId]: m_PeersByConnection) {
				(void)peerId;
				if (m_Interface->GetDetailedConnectionStatus(connection, detail.data(), static_cast<int>(detail.size())) != 0) continue;
				const NetFakeLinkEffects one = GnsTransport::ParseFakeLinkEffects(detail.data());
				total.jitterPackets += one.jitterPackets;
				total.reorderedPackets += one.reorderedPackets;
				total.duplicatedPackets += one.duplicatedPackets;
			}
			return total;
		}

		std::string GetLocalIdentity() {
			SteamNetworkingIdentity identity;
			if (!m_Interface || !m_Interface->GetIdentity(&identity)) {
				return {};
			}
			char text[SteamNetworkingIdentity::k_cchMaxString] = {};
			identity.ToString(text, sizeof(text));
			return text;
		}

		// ResetIdentity closes every connection of the process-wide interface, so it only runs on a change.
		bool ApplyP2PIdentity(const GnsP2PConfig& config, std::string* error) {
			if (config.localIdentity.empty()) {
				return true;
			}
			SteamNetworkingIdentity wanted;
			if (!wanted.ParseString(config.localIdentity.c_str())) {
				SetError(error, "GNS could not parse local identity '" + config.localIdentity + "'");
				return false;
			}
			SteamNetworkingIdentity current;
			if (!m_Interface->GetIdentity(&current) || !(current == wanted)) {
				if (const std::string live = DescribeLiveGnsObjects(); !live.empty()) {
					SetError(error, "GNS will not ResetIdentity to '" + config.localIdentity + "' while this process has " + live + " open: the identity is set once, before the first connection");
					return false;
				}
				m_Interface->ResetIdentity(&wanted);
			}
			return true;
		}

		// ResetIdentity destroys every connection and listen socket of the process, not only this transport's.
		static std::string DescribeLiveGnsObjects() {
			size_t listenSockets = 0;
			for (const Impl* impl : s_LiveImpls) {
				listenSockets += impl->m_ListenSocket != k_HSteamListenSocket_Invalid ? 1 : 0;
			}
			if (s_ConnectionOwners.empty() && listenSockets == 0) {
				return {};
			}
			return std::to_string(s_ConnectionOwners.size()) + " connection(s) and " + std::to_string(listenSockets) + " listen socket(s)";
		}

		void UpdateListenerIceServers(const GnsP2PConfig& config) {
			GnsTransport::ApplyIceServers(config);
			UpdateLiveTurnLogins(config);
			m_RelayOffer = config.relayOffer;
			if (m_ListenSocket == k_HSteamListenSocket_Invalid) return;
			auto* utils = SteamNetworkingUtils();
			const int32 iceEnable = GatheredIceEnable(config.iceEnable);
			utils->SetConfigValue(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_ESteamNetworkingConfig_ListenSocket, m_ListenSocket, k_ESteamNetworkingConfig_Int32, &iceEnable);
			utils->SetConfigValue(k_ESteamNetworkingConfig_P2P_TURN_ServerList, k_ESteamNetworkingConfig_ListenSocket, m_ListenSocket, k_ESteamNetworkingConfig_String, config.turnServerList.c_str());
			utils->SetConfigValue(k_ESteamNetworkingConfig_P2P_TURN_UserList, k_ESteamNetworkingConfig_ListenSocket, m_ListenSocket, k_ESteamNetworkingConfig_String, config.turnUserList.c_str());
			utils->SetConfigValue(k_ESteamNetworkingConfig_P2P_TURN_PassList, k_ESteamNetworkingConfig_ListenSocket, m_ListenSocket, k_ESteamNetworkingConfig_String, config.turnPassList.c_str());
		}

		// A renewed relay login reaches the TURN allocation of every live P2P connection.
		void UpdateLiveTurnLogins(const GnsP2PConfig& config) {
			const std::string login = config.turnServerList + '\n' + config.turnUserList + '\n' + config.turnPassList;
			if (m_P2PMode < 0 || !m_Interface || config.turnServerList.empty() || login == m_LiveTurnLogin) return;
			m_LiveTurnLogin = login;
			auto* utils = SteamNetworkingUtils();
			int renewed = 0;
			for (const auto& [connection, peerId] : m_PeersByConnection) {
				(void)peerId;
				m_ConnectionOffers[connection] = config.relayOffer;
				renewed += utils->SetConfigValue(k_ESteamNetworkingConfig_P2P_TURN_ServerList, k_ESteamNetworkingConfig_Connection, connection, k_ESteamNetworkingConfig_String, config.turnServerList.c_str()) &&
				           utils->SetConfigValue(k_ESteamNetworkingConfig_P2P_TURN_UserList, k_ESteamNetworkingConfig_Connection, connection, k_ESteamNetworkingConfig_String, config.turnUserList.c_str()) &&
				           utils->SetConfigValue(k_ESteamNetworkingConfig_P2P_TURN_PassList, k_ESteamNetworkingConfig_Connection, connection, k_ESteamNetworkingConfig_String, config.turnPassList.c_str());
			}
			if (renewed > 0) DiagnosticLine() << "[net-relay] relay login renewed on " << renewed << " live connection(s)" << std::endl;
		}

		/// CC_TEST_ICE_GATHER_RELAY_ONLY=1 stands for a network no direct route can cross: this end offers relay candidates only, while
		/// the player's Connection setting, and so the route policy, stays what it is.
		static int32 GatheredIceEnable(int iceEnable) {
			const char* lever = std::getenv("CC_TEST_ICE_GATHER_RELAY_ONLY");
			return lever && std::string_view(lever) == "1" ? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay : iceEnable;
		}

		static std::vector<SteamNetworkingConfigValue_t> P2PConnectionConfigs(const GnsP2PConfig& config) {
			GnsTransport::ApplyIceServers(config);
			std::vector<SteamNetworkingConfigValue_t> connectionConfigs(11);
			connectionConfigs[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(SteamNetConnectionStatusChangedCallback));
			// The IP path's send budget and connected timeout, for the same reasons.
			connectionConfigs[1].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, 8 * 1024 * 1024);
			connectionConfigs[2].SetInt32(k_ESteamNetworkingConfig_SendRateMin, 2 * 1024 * 1024);
			connectionConfigs[3].SetInt32(k_ESteamNetworkingConfig_SendRateMax, 32 * 1024 * 1024);
			connectionConfigs[4].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, c_NetLinkTimeoutMs);
			connectionConfigs[5].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, GatheredIceEnable(config.iceEnable));
			connectionConfigs[6].SetString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, config.stunServerList.c_str());
			connectionConfigs[7].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Implementation, config.iceImplementation);
			connectionConfigs[8].SetString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, config.turnServerList.c_str());
			connectionConfigs[9].SetString(k_ESteamNetworkingConfig_P2P_TURN_UserList, config.turnUserList.c_str());
			connectionConfigs[10].SetString(k_ESteamNetworkingConfig_P2P_TURN_PassList, config.turnPassList.c_str());
			// Candidates cross the directory before a route can be tried, so a relayed connect outlasts GNS's 10 s default.
			connectionConfigs.emplace_back();
			connectionConfigs.back().SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, static_cast<int32>(GnsTransport::IceConnectTimeoutMs()));
			if (config.rendezvousLogLevel > 0) {
				connectionConfigs.emplace_back();
				connectionConfigs.back().SetInt32(k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, config.rendezvousLogLevel);
			}
			return connectionConfigs;
		}

		static int ConnectionConfigInt32(HSteamNetConnection connection, ESteamNetworkingConfigValue value) {
			int32 number = -1;
			size_t size = sizeof(number);
			ESteamNetworkingConfigDataType type = k_ESteamNetworkingConfig_Int32;
			SteamNetworkingUtils()->GetConfigValue(value, k_ESteamNetworkingConfig_Connection, connection, &type, &number, &size);
			return number;
		}

		static std::string ConnectionConfigString(HSteamNetConnection connection, ESteamNetworkingConfigValue value) {
			char text[1024] = {};
			size_t size = sizeof(text);
			ESteamNetworkingConfigDataType type = k_ESteamNetworkingConfig_String;
			if (SteamNetworkingUtils()->GetConfigValue(value, k_ESteamNetworkingConfig_Connection, connection, &type, text, &size) < k_ESteamNetworkingGetConfigValue_OK) {
				return {};
			}
			return text;
		}

		// Routes the callbacks of a connection that a peer's request creates to the transport that received it.
		struct OwningRecvContext final : ISteamNetworkingSignalingRecvContext {
			OwningRecvContext(Impl* owner, ISteamNetworkingSignalingRecvContext* inner) : m_Owner(owner), m_Inner(inner) {}

			ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection connection, const SteamNetworkingIdentity& identityPeer, int localVirtualPort) override {
				ISteamNetworkingConnectionSignaling* signaling = m_Inner ? m_Inner->OnConnectRequest(connection, identityPeer, localVirtualPort) : nullptr;
				if (signaling) {
					s_ConnectionOwners[connection] = m_Owner;
				}
				return signaling;
			}

			void SendRejectionSignal(const SteamNetworkingIdentity& identityPeer, const void* message, int size) override {
				if (m_Inner) {
					const std::vector<uint8_t> named = NameRejectionSender(message, size, m_Owner ? m_Owner->GetLocalIdentity() : std::string());
					m_Inner->SendRejectionSignal(identityPeer, named.data(), static_cast<int>(named.size()));
				}
			}

			Impl* m_Owner;
			ISteamNetworkingSignalingRecvContext* m_Inner;
		};

		/// GNS 1.6.0 marshals a rejection that names no sender, and a receiver drops any signal that names none, so the
		/// refused joiner waits out its connect timeout. The rendezvous message's from_identity (field 8) is added when absent.
		static std::vector<uint8_t> NameRejectionSender(const void* message, int size, const std::string& identity) {
			const uint8_t* bytes = static_cast<const uint8_t*>(message);
			std::vector<uint8_t> named(bytes, bytes + std::max(size, 0));
			const auto readVarint = [&named](size_t& at, uint64_t& value) {
				value = 0;
				for (int shift = 0; shift < 64 && at < named.size(); shift += 7) {
					const uint8_t byte = named[at++];
					value |= static_cast<uint64_t>(byte & 0x7F) << shift;
					if ((byte & 0x80) == 0) return true;
				}
				return false;
			};
			if (identity.empty() || identity.size() > 127) return named;
			for (size_t at = 0; at < named.size();) {
				uint64_t key = 0;
				uint64_t length = 0;
				if (!readVarint(at, key)) return named;
				if ((key >> 3) == 8) return named;
				switch (key & 7) {
					case 0: if (!readVarint(at, length)) return named; break;
					case 1: at += 8; break;
					case 2: if (!readVarint(at, length) || length > named.size() - at) return named; at += static_cast<size_t>(length); break;
					case 5: at += 4; break;
					default: return named;
				}
				if (at > named.size()) return named;
			}
			named.push_back(static_cast<uint8_t>((8 << 3) | 2));
			named.push_back(static_cast<uint8_t>(identity.size()));
			named.insert(named.end(), identity.begin(), identity.end());
			return named;
		}

		bool m_HasGnsRef = false;
		bool m_IsHost = false;
		bool m_IsStarted = false;
		int m_P2PMode = -1;
		ISteamNetworkingSockets* m_Interface = nullptr;
		HSteamListenSocket m_ListenSocket = k_HSteamListenSocket_Invalid;
		HSteamNetPollGroup m_PollGroup = k_HSteamNetPollGroup_Invalid;
		HSteamNetConnection m_ServerConnection = k_HSteamNetConnection_Invalid;
		NetPeerId m_NextPeerId = 1;
		std::map<HSteamNetConnection, NetPeerId> m_PeersByConnection;
		std::map<NetPeerId, HSteamNetConnection> m_ConnectionsByPeer;
		std::vector<NetTransportEvent> m_PendingEvents;
		std::map<NetPeerId, uint64_t> m_BytesHandedOver; //!< What we actually gave the socket, to read the pending figure against.
		std::map<HSteamNetConnection, SteamNetworkingMicroseconds> m_LastDetailUs; //!< When each connection last produced a detailed status.
		std::map<HSteamNetConnection, SteamNetworkingMicroseconds> m_LastDeliveryTraceUs;
		std::set<HSteamNetConnection> m_Announced; //!< Connections whose PeerConnected we have already handed up.
		std::map<HSteamNetConnection, std::vector<NetTransportEvent>> m_HeldPackets; //!< Payloads GNS delivered before that.
		std::string m_LiveTurnLogin; //!< The TURN server, user and password lists the live connections run with.
		const GnsTransport* m_Facade = nullptr; //!< The transport this implements, as the process's connection list names it.

		static std::map<HSteamListenSocket, Impl*> s_ListenerOwners;
		static std::map<HSteamNetConnection, Impl*> s_ConnectionOwners;
		static std::set<const Impl*> s_LiveImpls;

		// Lists every transport for the identity guard without a change to the IP path's code.
		struct LiveImplRegistration {
			explicit LiveImplRegistration(const Impl* impl) : m_Impl(impl) { s_LiveImpls.insert(m_Impl); }
			~LiveImplRegistration() { s_LiveImpls.erase(m_Impl); }
			const Impl* m_Impl;
		} m_LiveImplRegistration{this};
	};

	std::map<HSteamListenSocket, GnsTransport::Impl*> GnsTransport::Impl::s_ListenerOwners;
	std::map<HSteamNetConnection, GnsTransport::Impl*> GnsTransport::Impl::s_ConnectionOwners;
	std::set<const GnsTransport::Impl*> GnsTransport::Impl::s_LiveImpls;

#else

	struct GnsTransport::Impl {
		bool StartHost(uint16_t, std::string* error) {
			SetError(error, "GameNetworkingSockets support is not compiled in; rebuild with CCCP_WITH_GNS");
			return false;
		}

		bool Connect(const std::string&, uint16_t, std::string* error) {
			SetError(error, "GameNetworkingSockets support is not compiled in; rebuild with CCCP_WITH_GNS");
			return false;
		}

		bool Send(NetPeerId, NetTransportLane, const std::vector<uint8_t>&, std::string* error, bool* congested) {
			if (congested) *congested = false;
			SetError(error, "GameNetworkingSockets support is not compiled in; rebuild with CCCP_WITH_GNS");
			return false;
		}

		void Disconnect(NetPeerId, const std::string&) {}
		void Stop() {}
		std::vector<NetTransportEvent> PollEvents() { return {}; }
		uint32_t GetPeerPingMs(NetPeerId) { return 0; }
		bool IsPeerPingMeasured(NetPeerId) { return false; }

		bool StartHostP2P(int, const GnsP2PConfig&, std::string* error) {
			SetError(error, "GameNetworkingSockets support is not compiled in; rebuild with CCCP_WITH_GNS");
			return false;
		}

		bool ConnectP2P(ISteamNetworkingConnectionSignaling*, const std::string&, int, const GnsP2PConfig&, std::string* error) {
			SetError(error, "GameNetworkingSockets support is not compiled in; rebuild with CCCP_WITH_GNS");
			return false;
		}

		bool ReceiveP2PSignal(const void*, int, ISteamNetworkingSignalingRecvContext*) { return false; }
		GnsPeerConnectionInfo GetPeerConnectionInfo(NetPeerId) { return {}; }
		static std::vector<GnsProcessConnection> ProcessConnections() { return {}; }
		std::string GetPeerDetailedStatus(NetPeerId) { return {}; }
		NetFakeLinkEffects GetFakeLinkEffects() { return {}; }
		std::string GetLocalIdentity() { return {}; }
		const GnsTransport* m_Facade = nullptr;
	};

#endif

	namespace {
		// One lock for every call into any transport: GNS dispatches a connection's callbacks from whichever transport pumps, and the
		// session thread may send beside the simulation thread's pump.
		std::recursive_mutex& GnsCallLock() {
			static std::recursive_mutex lock;
			return lock;
		}
	}

	// Under the call lock, because the process's connection list walks every live transport.
	GnsTransport::GnsTransport() {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		m_Impl = new Impl();
		m_Impl->m_Facade = this;
	}

	GnsTransport::~GnsTransport() {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		delete m_Impl;
		m_Impl = nullptr;
	}

	bool GnsTransport::StartHost(uint16_t port, std::string* error) {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		if (SettingsMan::IsConstructed() && g_SettingsMan.GetNetworkConnectionMode() == SettingsMan::NetworkConnectionMode::RelayOnly) {
			m_Impl->Stop();
			SetError(error, "Your Connection is Relay only. Host an Internet game, or switch Connection to Automatic to listen on a direct address.");
			return false;
		}
		return m_Impl->StartHost(port, error);
	}

	bool GnsTransport::Connect(const std::string& address, uint16_t port, std::string* error) {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		if (SettingsMan::IsConstructed() && g_SettingsMan.GetNetworkConnectionMode() == SettingsMan::NetworkConnectionMode::RelayOnly) {
			m_Impl->Stop();
			SetError(error, "Your Connection is Relay only. Join from the Internet list, or switch Connection to Automatic to use a direct address.");
			return false;
		}
		return m_Impl->Connect(address, port, error);
	}

	bool GnsTransport::Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->Send(peerId, lane, bytes, error, congested);
	}

	void GnsTransport::Disconnect(NetPeerId peerId, const std::string& reason) {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		m_Impl->Disconnect(peerId, reason);
	}

	void GnsTransport::Stop() {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		m_Impl->Stop();
	}

	std::vector<NetTransportEvent> GnsTransport::PollEvents() {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->PollEvents();
	}

	uint32_t GnsTransport::GetPeerPingMs(NetPeerId peerId) const {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->GetPeerPingMs(peerId);
	}

	bool GnsTransport::IsPeerPingMeasured(NetPeerId peerId) const {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->IsPeerPingMeasured(peerId);
	}

	bool GnsTransport::StartHostP2P(int virtualPort, const GnsP2PConfig& config, std::string* error) {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->StartHostP2P(virtualPort, config, error);
	}

	bool GnsTransport::ConnectP2P(ISteamNetworkingConnectionSignaling* signaling, const std::string& peerIdentity, int remoteVirtualPort, const GnsP2PConfig& config, std::string* error) {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->ConnectP2P(signaling, peerIdentity, remoteVirtualPort, config, error);
	}

	bool GnsTransport::ReceiveP2PSignal(const void* blob, int size, ISteamNetworkingSignalingRecvContext* context) {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->ReceiveP2PSignal(blob, size, context);
	}

	GnsPeerConnectionInfo GnsTransport::GetPeerConnectionInfo(NetPeerId peerId) const {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->GetPeerConnectionInfo(peerId);
	}

	std::vector<GnsProcessConnection> GnsTransport::GetProcessConnections() {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return Impl::ProcessConnections();
	}

	std::string GnsTransport::GetPeerDetailedStatus(NetPeerId peerId) const {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->GetPeerDetailedStatus(peerId);
	}

	NetFakeLinkEffects GnsTransport::GetFakeLinkEffects() const {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		return m_Impl->GetFakeLinkEffects();
	}

	NetFakeLinkEffects GnsTransport::ParseFakeLinkEffects(const std::string& status) {
		NetFakeLinkEffects effects;
		// The connection's own end prints first; the remote host's copy of its counters follows.
		const size_t local = status.find("Lifetime stats:");
		if (local == std::string::npos) return effects;
		const size_t remote = status.find("received from remote host", local);
		const std::string_view section = std::string_view(status).substr(local, remote == std::string::npos ? std::string::npos : remote - local);
		// GNS groups thousands with commas.
		const auto number = [&section](size_t& at) -> int64_t {
			while (at < section.size() && section[at] == ' ') ++at;
			int64_t value = 0;
			for (; at < section.size() && ((section[at] >= '0' && section[at] <= '9') || section[at] == ','); ++at)
				if (section[at] != ',') value = value * 10 + (section[at] - '0');
			return value;
		};
		const auto counter = [&](std::string_view label) -> int64_t {
			size_t at = section.find(label);
			if (at == std::string_view::npos) return 0;
			at += label.size();
			return number(at);
		};
		effects.reorderedPackets = counter("OutOfOrder:");
		effects.duplicatedPackets = counter("Duplicate :");
		// The latency variance histogram's counts follow its header row: under 1 ms, then 1-2, 2-5, 5-10, 10-20 and over 20.
		if (const size_t histogram = section.find("Latency variance histogram"); histogram != std::string_view::npos) {
			const size_t header = section.find('\n', histogram);
			const size_t counts = header == std::string_view::npos ? std::string_view::npos : section.find('\n', header + 1);
			if (counts != std::string_view::npos) {
				size_t at = counts + 1;
				for (int bucket = 0; bucket < 6; ++bucket) {
					const int64_t value = number(at);
					if (bucket > 0) effects.jitterPackets += value;
				}
			}
		}
		return effects;
	}

	void GnsTransport::GetFakeLinkSettings(int& jitterMs, float& reorderPercent, float& duplicatePercent) {
		jitterMs = s_SimulatedJitterMs;
		reorderPercent = s_SimulatedReorderPercent;
		duplicatePercent = s_SimulatedDuplicatePercent;
	}

	std::string GnsTransport::GetLocalIdentity() const {
		return m_Impl->GetLocalIdentity();
	}

	uint32_t GnsTransport::LocalRouteRevision() {
#ifdef CCCP_WITH_GNS
		return SteamNetworkingSockets_GetLocalRouteRevision();
#else
		return 0;
#endif
	}

	std::string GnsTransport::ProcessIdentity() {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
#ifdef CCCP_WITH_GNS
		SteamNetworkingIdentity identity;
		if (!g_GnsInitialized || !SteamNetworkingSockets() || !SteamNetworkingSockets()->GetIdentity(&identity)) return {};
		char text[SteamNetworkingIdentity::k_cchMaxString] = {};
		identity.ToString(text, sizeof(text));
		return text;
#else
		return {};
#endif
	}

	bool GnsTransport::IsCompiledIn() {
#ifdef CCCP_WITH_GNS
		return true;
#else
		return false;
#endif
	}

	uint32_t GnsTransport::IceConnectTimeoutMs() {
		if (const char* lever = std::getenv("CC_TEST_ICE_CONNECT_TIMEOUT_MS"); lever && *lever) {
			char* end = nullptr;
			const unsigned long value = std::strtoul(lever, &end, 10);
			if (end && *end == '\0' && value >= 1000 && value <= 600000) return static_cast<uint32_t>(value);
		}
		return c_IceConnectTimeoutMs;
	}

	void GnsTransport::UpdateListenerIceServers(const GnsP2PConfig& config) {
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
#ifdef CCCP_WITH_GNS
		m_Impl->UpdateListenerIceServers(config);
#else
		(void)config;
#endif
	}

	void GnsTransport::ApplyIceServers(const GnsP2PConfig& config) {
		// Every relay login reaches the transport through here, so every log line can be scrubbed of it.
		NetRelayLogins::Remember(config.turnUserList);
		NetRelayLogins::Remember(config.turnPassList);
#ifdef CCCP_WITH_GNS
		if (!SteamNetworkingUtils()) return;
		SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, config.stunServerList.c_str());
		SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, config.turnServerList.c_str());
		SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_UserList, config.turnUserList.c_str());
		SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_PassList, config.turnPassList.c_str());
#else
		(void)config;
#endif
	}

	void GnsTransport::SetSimulatedLagMs(int lagMs) {
#ifdef CCCP_WITH_GNS
		s_SimulatedLagMs = lagMs;
#else
		(void)lagMs;
#endif
	}

	void GnsTransport::SetSimulatedReorderPercent(float percent) {
#ifdef CCCP_WITH_GNS
		// A harness lever: only a headless run reorders its packets.
		const char* headless = std::getenv("CCCP_HEADLESS");
		s_SimulatedReorderPercent = headless && std::string_view(headless) == "1" && percent >= 0 && percent <= 100 ? percent : 0;
#else
		(void)percent;
#endif
	}

	void GnsTransport::SetSimulatedDuplicatePercent(float percent) {
#ifdef CCCP_WITH_GNS
		// A harness lever: only a headless run duplicates its packets.
		const char* headless = std::getenv("CCCP_HEADLESS");
		s_SimulatedDuplicatePercent = headless && std::string_view(headless) == "1" && percent >= 0 && percent <= 100 ? percent : 0;
#else
		(void)percent;
#endif
	}

	void GnsTransport::SetSimulatedJitterMs(int jitterMs) {
#ifdef CCCP_WITH_GNS
		// A harness lever: only a headless run jitters its links, within a cross fault's range.
		const char* headless = std::getenv("CCCP_HEADLESS");
		s_SimulatedJitterMs = headless && std::string_view(headless) == "1" && jitterMs >= 0 && jitterMs <= 10000 ? jitterMs : 0;
#else
		(void)jitterMs;
#endif
	}

	bool ApplyCrossTransportFault(int lagMs, float lossPercent, float jitterMs, uint64_t durationMs) {
#ifdef CCCP_WITH_GNS
		const char* headless = std::getenv("CCCP_HEADLESS");
		if (!headless || std::string_view(headless) != "1" || lagMs < 0 || lagMs > 20000 ||
		    !std::isfinite(lossPercent) || !std::isfinite(jitterMs) || lossPercent < 0 || lossPercent > 100 || jitterMs < 0 || jitterMs > 10000 || durationMs > 600000) return false;
		auto* utils = SteamNetworkingUtils();
		if (!utils) return false;
		bool accepted = true;
		accepted &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, lagMs / 2);
		accepted &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, lagMs - lagMs / 2);
		accepted &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, lossPercent);
		accepted &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, lossPercent);
		accepted &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, jitterMs / 2);
		accepted &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Avg, jitterMs / 2);
		accepted &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, jitterMs);
		accepted &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Max, jitterMs);
		accepted &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, jitterMs > 0 ? 100.0F : 0.0F);
		accepted &= utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Recv_Pct, jitterMs > 0 ? 100.0F : 0.0F);
		// A jittered cross link sends its packet spacing as the jitter lever's does; with no jitter left, GNS's default returns.
		accepted &= utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendTimeSincePreviousPacket, jitterMs > 0 || s_SimulatedJitterMs > 0 ? 1 : -1);
		s_CrossTransportResetMs.store(durationMs ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + durationMs : 0);
		return accepted;
#else
		(void)lagMs; (void)lossPercent; (void)jitterMs; (void)durationMs;
		return false;
#endif
	}

	bool GnsPacketSpacingSelfTest(std::string* error) {
#ifdef CCCP_WITH_GNS
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		if (!AcquireGns(error)) return false;
		const auto spacing = [] {
			int32 value = 0;
			size_t size = sizeof(value);
			ESteamNetworkingConfigDataType type = k_ESteamNetworkingConfig_Int32;
			SteamNetworkingUtils()->GetConfigValue(k_ESteamNetworkingConfig_SendTimeSincePreviousPacket, k_ESteamNetworkingConfig_Global, 0, &type, &value, &size);
			return value;
		};
		// A transport starts the way a player's does, then the way the jitter lever's does.
		const int lever = s_SimulatedJitterMs;
		s_SimulatedJitterMs = 0;
		ApplySimulatedLag();
		const int32 plain = spacing();
		s_SimulatedJitterMs = 40;
		ApplySimulatedLag();
		const int32 jittered = spacing();
		s_SimulatedJitterMs = lever;
		// The rest of the process runs on GNS's defaults again.
		SteamNetworkingUtils()->SetConfigValue(k_ESteamNetworkingConfig_SendTimeSincePreviousPacket, k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, nullptr);
		for (const ESteamNetworkingConfigValue value: {k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, k_ESteamNetworkingConfig_FakePacketJitter_Recv_Avg,
		                                               k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, k_ESteamNetworkingConfig_FakePacketJitter_Recv_Max,
		                                               k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, k_ESteamNetworkingConfig_FakePacketJitter_Recv_Pct}) {
			SteamNetworkingUtils()->SetConfigValue(value, k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Float, nullptr);
		}
		ReleaseGns();
		const char* headless = std::getenv("CCCP_HEADLESS");
		const std::string run = std::string("CCCP_HEADLESS=") + (headless ? headless : "(unset)");
		if (plain != -1) {
			if (error) *error = "a transport with no jitter lever set SendTimeSincePreviousPacket=" + std::to_string(plain) + " under " + run + ", not GNS's default -1";
			return false;
		}
		if (jittered != 1) {
			if (error) *error = "the jitter lever's transport set SendTimeSincePreviousPacket=" + std::to_string(jittered) + ", so the jitter receipt has no packet spacing to count";
			return false;
		}
		return true;
#else
		if (error) *error = "GameNetworkingSockets support is not compiled in";
		return false;
#endif
	}

	void GnsTransport::SetRendezvousLogLevel(int level) {
#ifdef CCCP_WITH_GNS
		s_RendezvousLogLevel = level;
#else
		(void)level;
#endif
	}

	void GnsTransport::ObserveOutgoingLockstepFrame(uint64_t targetFrame) {
#ifdef CCCP_WITH_GNS
		const auto& config = TestUplinkStallConfig();
		if (config.durationMs == 0) return;
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		if (SteamNetworkingUtils()) s_TestUplinkStall.NoteFrame(config, targetFrame, UplinkStallClockMs());
#else
		(void)targetFrame;
#endif
	}

	bool GnsTransport::UplinkStallSelfTest(std::string* error) {
#ifdef CCCP_WITH_GNS
		std::lock_guard<std::recursive_mutex> lock(GnsCallLock());
		if (!AcquireGns(error)) return false;
		bool passed = true;
		const float send = GlobalLoss(k_ESteamNetworkingConfig_FakePacketLoss_Send), receive = GlobalLoss(k_ESteamNetworkingConfig_FakePacketLoss_Recv);
		for (uint64_t duration : {300U, 800U}) {
			UplinkStall test;
			const UplinkStallConfig config{600, duration};
			passed = passed && !test.NoteFrame({}, 600, 1000) && !test.NoteFrame(config, 599, 1000) && test.NoteFrame(config, 600, 1000);
			test.Update(1000 + duration - 1);
			passed = passed && GlobalLoss(k_ESteamNetworkingConfig_FakePacketLoss_Send) == 100.0F && GlobalLoss(k_ESteamNetworkingConfig_FakePacketLoss_Recv) == receive;
			test.Update(1000 + duration);
			passed = passed && test.finished && GlobalLoss(k_ESteamNetworkingConfig_FakePacketLoss_Send) == send && !test.NoteFrame(config, 600, 2000);
		}
		for (const auto& bad : {ParseUplinkStall(nullptr, "300"), ParseUplinkStall("600", nullptr), ParseUplinkStall("-1", "300"), ParseUplinkStall("600x", "300"), ParseUplinkStall("600", "10001")}) passed = passed && bad.durationMs == 0;
		passed = passed && ParseUplinkStall("600", "300").durationMs == 300;
		SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, send);
		ReleaseGns();
		if (!passed && error) *error = "the one-way stall changed receiving, failed its clock, or failed to restore sending";
		return passed;
#else
		if (error) *error = "GameNetworkingSockets support is not compiled in";
		return false;
#endif
	}

	bool GnsTransport::PayloadHoldSelfTest(std::string* error) {
#ifdef CCCP_WITH_GNS
		GnsTransport transport;
		if (!transport.m_Impl->PayloadHoldSelfTest(error)) return false;
		std::map<HSteamNetConnection, std::vector<NetTransportEvent>> held;
		const HSteamNetConnection connection = static_cast<HSteamNetConnection>(41);
		NetTransportEvent first{NetTransportEventType::PacketReceived, 2, NetTransportLane::ControlReliable, {1, 2, 3}, {}};
		NetTransportEvent second{NetTransportEventType::PacketReceived, 2, NetTransportLane::InputUnreliable, {4, 5}, {}};
		if (!QueueHeldPacket(held, connection, std::move(first)) || !QueueHeldPacket(held, connection, std::move(second))) {
			if (error) *error = "the payload hold rejected a packet below its cap: first_bytes=3 second_bytes=2 cap=" + std::to_string(c_MaxHeldBytesBeforeAnnounce);
			return false;
		}
		const auto found = held.find(connection);
		if (found == held.end() || found->second.size() != 2 || found->second[0].bytes != std::vector<uint8_t>({1, 2, 3}) ||
		    found->second[1].bytes != std::vector<uint8_t>({4, 5})) {
			if (error) *error = "the payload hold did not preserve pre-announcement order: connections=" + std::to_string(held.size()) +
			                  " packets=" + std::to_string(found == held.end() ? 0 : found->second.size());
			return false;
		}
		NetTransportEvent oversized{NetTransportEventType::PacketReceived, 2, NetTransportLane::ControlReliable,
		                            std::vector<uint8_t>(c_MaxHeldBytesBeforeAnnounce, 0), {}};
		const size_t attemptedBytes = 5 + oversized.bytes.size();
		const bool acceptedOversized = QueueHeldPacket(held, connection, std::move(oversized));
		if (acceptedOversized || HeldPacketOverflowReason(attemptedBytes).find(std::to_string(attemptedBytes)) == std::string::npos ||
		    HeldPacketOverflowReason(attemptedBytes).find(std::to_string(c_MaxHeldBytesBeforeAnnounce)) == std::string::npos) {
			if (error) *error = "the payload hold did not enforce and name its byte cap: attempted_bytes=" + std::to_string(attemptedBytes) +
			                  " cap=" + std::to_string(c_MaxHeldBytesBeforeAnnounce) + " accepted=" +
			                  std::to_string(acceptedOversized);
			return false;
		}
		return true;
#else
		if (error) *error = "GameNetworkingSockets support is not compiled in";
		return false;
#endif
	}

} // namespace RTE
