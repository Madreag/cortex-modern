#include "GnsTransport.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <utility>

#ifdef CCCP_WITH_GNS
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <steam/steamnetworkingsockets.h>
#endif

namespace RTE {

	namespace {
		void SetError(std::string* error, const std::string& message) {
			if (error) {
				*error = message;
			}
		}
	}

#ifdef CCCP_WITH_GNS

	namespace {
		bool g_GnsInitialized = false;
		uint32_t g_GnsRefCount = 0;

		bool AcquireGns(std::string* error) {
			if (!g_GnsInitialized) {
				SteamDatagramErrMsg initError;
				if (!GameNetworkingSockets_Init(nullptr, initError)) {
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
			}
			return k_nSteamNetworkingSend_Reliable;
		}

		std::string EndDebugText(const SteamNetConnectionStatusChangedCallback_t& info) {
			return info.m_info.m_szEndDebug[0] != '\0' ? info.m_info.m_szEndDebug : "GNS connection closed";
		}

		int s_SimulatedLagMs = 0;

		// Test harness: splits the requested RTT across the send/recv legs of every connection.
		void ApplySimulatedLag() {
			if (s_SimulatedLagMs <= 0) {
				return;
			}
			SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, s_SimulatedLagMs / 2);
			SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, s_SimulatedLagMs - s_SimulatedLagMs / 2);
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
			connectionConfigs[4].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 4000);

			m_Interface = SteamNetworkingSockets();
			m_ListenSocket = m_Interface->CreateListenSocketIP(listenAddress, 5, connectionConfigs);
			if (m_ListenSocket == k_HSteamListenSocket_Invalid) {
				SetError(error, "CreateListenSocketIP failed");
				return false;
			}

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
			connectionConfigs[4].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 4000);

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
			return true;
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
			m_HasLingeringClose = true;
			ForgetConnection(connection);
			// GNS reports nothing for a close we made ourselves, and forgetting the handle means its own
			// later callback finds no peer either. A peer leaving must look the same to us however it
			// went, or state keyed on the connection - a held seat, most of all - is never cleaned up.
			m_PendingEvents.push_back({NetTransportEventType::PeerDisconnected, peerId, NetTransportLane::ControlReliable, {}, reason});
		}

		void Stop() {
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
				m_HasLingeringClose = true;
				ForgetConnection(connection);
			}

			// A lingering close transmits on the GNS service thread; give it a beat before teardown.
			if (m_HasLingeringClose) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				m_HasLingeringClose = false;
			}

			if (m_ListenSocket != k_HSteamListenSocket_Invalid) {
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
			m_PendingEvents.clear();
		}

		std::vector<NetTransportEvent> PollEvents() {
			if (m_Interface) {
				// Drain delivered messages first: a close callback forgets the connection, which would
				// drop a reject/goodbye that GNS already delivered alongside it.
				PollIncomingMessages();
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
			s_CallbackInstance = this;
			m_Interface->RunCallbacks();
			if (s_CallbackInstance == this) {
				s_CallbackInstance = nullptr;
			}
		}

		void PollIncomingMessages() {
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
				if (peerIt != m_PeersByConnection.end()) {
					const uint8_t* data = static_cast<const uint8_t*>(message->m_pData);
					m_PendingEvents.push_back({
						NetTransportEventType::PacketReceived,
						peerIt->second,
						LaneFromMessage(*message),
						std::vector<uint8_t>(data, data + message->m_cbSize),
						{}});
				}
				message->Release();
			}
		}

		void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info) {
			if (!info || !m_Interface) {
				return;
			}

			switch (info->m_info.m_eState) {
				case k_ESteamNetworkingConnectionState_Connecting:
					if (m_IsHost && info->m_info.m_hListenSocket == m_ListenSocket) {
						AcceptIncomingConnection(info->m_hConn);
					}
					break;
				case k_ESteamNetworkingConnectionState_Connected:
					if (!m_IsHost && info->m_hConn == m_ServerConnection) {
						EnsureClientConnected(info->m_hConn);
					}
					break;
				case k_ESteamNetworkingConnectionState_ClosedByPeer:
				case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
					HandleConnectionClosed(info->m_hConn, EndDebugText(*info), info->m_eOldState);
					break;
				case k_ESteamNetworkingConnectionState_None:
				default:
					break;
			}
		}

		void AcceptIncomingConnection(HSteamNetConnection connection) {
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
			m_PendingEvents.push_back({NetTransportEventType::PeerConnected, peerId, NetTransportLane::ControlReliable, {}, {}});
		}

		void EnsureClientConnected(HSteamNetConnection connection) {
			if (m_PeersByConnection.find(connection) == m_PeersByConnection.end()) {
				m_PeersByConnection[connection] = 1;
				m_ConnectionsByPeer[1] = connection;
				s_ConnectionOwners[connection] = this;
			}
			m_PendingEvents.push_back({NetTransportEventType::PeerConnected, 1, NetTransportLane::ControlReliable, {}, {}});
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
			ForgetConnection(connection);
			m_PendingEvents.push_back({NetTransportEventType::PeerDisconnected, peerId, NetTransportLane::ControlReliable, {}, reason});
		}

		void ForgetConnection(HSteamNetConnection connection) {
			const auto peerIt = m_PeersByConnection.find(connection);
			if (peerIt != m_PeersByConnection.end()) {
				m_ConnectionsByPeer.erase(peerIt->second);
				m_BytesHandedOver.erase(peerIt->second);
				m_PeersByConnection.erase(peerIt);
			}
			s_ConnectionOwners.erase(connection);
			m_LastDetailUs.erase(connection);
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
			if (s_CallbackInstance) {
				s_CallbackInstance->OnConnectionStatusChanged(info);
			}
		}

		bool StartHostP2P(int virtualPort, const GnsP2PConfig& config, std::string* error) {
			Stop();
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
			std::vector<SteamNetworkingConfigValue_t> connectionConfigs = P2PConnectionConfigs(config);
			m_ListenSocket = m_Interface->CreateListenSocketP2P(virtualPort, static_cast<int>(connectionConfigs.size()), connectionConfigs.data());
			if (m_ListenSocket == k_HSteamListenSocket_Invalid) {
				SetError(error, "CreateListenSocketP2P failed");
				return false;
			}

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

		bool ConnectP2P(ISteamNetworkingConnectionSignaling* signaling, const std::string& peerIdentity, int remoteVirtualPort, const GnsP2PConfig& config, std::string* error) {
			Stop();
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
			return m_Interface->ReceivedP2PCustomSignal(blob, size, &owningContext);
		}

		GnsPeerConnectionInfo GetPeerConnectionInfo(NetPeerId peerId) {
			GnsPeerConnectionInfo result;
			const auto connectionIt = m_ConnectionsByPeer.find(peerId);
			SteamNetConnectionInfo_t info{};
			if (!m_Interface || connectionIt == m_ConnectionsByPeer.end() || !m_Interface->GetConnectionInfo(connectionIt->second, &info)) {
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
				result.config.push_back(std::string(name) + "=" + std::to_string(ConnectionConfigInt32(connectionIt->second, value)));
			}
			result.config.push_back("P2P_STUN_ServerList=\"" + ConnectionConfigString(connectionIt->second, k_ESteamNetworkingConfig_P2P_STUN_ServerList) + "\"");
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

		static std::vector<SteamNetworkingConfigValue_t> P2PConnectionConfigs(const GnsP2PConfig& config) {
			std::vector<SteamNetworkingConfigValue_t> connectionConfigs(8);
			connectionConfigs[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(SteamNetConnectionStatusChangedCallback));
			// The IP path's send budget and connected timeout, for the same reasons.
			connectionConfigs[1].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, 8 * 1024 * 1024);
			connectionConfigs[2].SetInt32(k_ESteamNetworkingConfig_SendRateMin, 2 * 1024 * 1024);
			connectionConfigs[3].SetInt32(k_ESteamNetworkingConfig_SendRateMax, 32 * 1024 * 1024);
			connectionConfigs[4].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, 4000);
			connectionConfigs[5].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, config.iceEnable);
			connectionConfigs[6].SetString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, config.stunServerList.c_str());
			connectionConfigs[7].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Implementation, config.iceImplementation);
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
					m_Inner->SendRejectionSignal(identityPeer, message, size);
				}
			}

			Impl* m_Owner;
			ISteamNetworkingSignalingRecvContext* m_Inner;
		};

		bool m_HasGnsRef = false;
		bool m_IsHost = false;
		bool m_IsStarted = false;
		bool m_HasLingeringClose = false;
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

		static Impl* s_CallbackInstance;
		static std::map<HSteamNetConnection, Impl*> s_ConnectionOwners;
		static std::set<const Impl*> s_LiveImpls;

		// Lists every transport for the identity guard without a change to the IP path's code.
		struct LiveImplRegistration {
			explicit LiveImplRegistration(const Impl* impl) : m_Impl(impl) { s_LiveImpls.insert(m_Impl); }
			~LiveImplRegistration() { s_LiveImpls.erase(m_Impl); }
			const Impl* m_Impl;
		} m_LiveImplRegistration{this};
	};

	GnsTransport::Impl* GnsTransport::Impl::s_CallbackInstance = nullptr;
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
		std::string GetPeerDetailedStatus(NetPeerId) { return {}; }
		std::string GetLocalIdentity() { return {}; }
	};

#endif

	GnsTransport::GnsTransport() : m_Impl(new Impl()) {}

	GnsTransport::~GnsTransport() {
		delete m_Impl;
		m_Impl = nullptr;
	}

	bool GnsTransport::StartHost(uint16_t port, std::string* error) {
		return m_Impl->StartHost(port, error);
	}

	bool GnsTransport::Connect(const std::string& address, uint16_t port, std::string* error) {
		return m_Impl->Connect(address, port, error);
	}

	bool GnsTransport::Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error, bool* congested) {
		return m_Impl->Send(peerId, lane, bytes, error, congested);
	}

	void GnsTransport::Disconnect(NetPeerId peerId, const std::string& reason) {
		m_Impl->Disconnect(peerId, reason);
	}

	void GnsTransport::Stop() {
		m_Impl->Stop();
	}

	std::vector<NetTransportEvent> GnsTransport::PollEvents() {
		return m_Impl->PollEvents();
	}

	uint32_t GnsTransport::GetPeerPingMs(NetPeerId peerId) const {
		return m_Impl->GetPeerPingMs(peerId);
	}

	bool GnsTransport::StartHostP2P(int virtualPort, const GnsP2PConfig& config, std::string* error) {
		return m_Impl->StartHostP2P(virtualPort, config, error);
	}

	bool GnsTransport::ConnectP2P(ISteamNetworkingConnectionSignaling* signaling, const std::string& peerIdentity, int remoteVirtualPort, const GnsP2PConfig& config, std::string* error) {
		return m_Impl->ConnectP2P(signaling, peerIdentity, remoteVirtualPort, config, error);
	}

	bool GnsTransport::ReceiveP2PSignal(const void* blob, int size, ISteamNetworkingSignalingRecvContext* context) {
		return m_Impl->ReceiveP2PSignal(blob, size, context);
	}

	GnsPeerConnectionInfo GnsTransport::GetPeerConnectionInfo(NetPeerId peerId) const {
		return m_Impl->GetPeerConnectionInfo(peerId);
	}

	std::string GnsTransport::GetPeerDetailedStatus(NetPeerId peerId) const {
		return m_Impl->GetPeerDetailedStatus(peerId);
	}

	std::string GnsTransport::GetLocalIdentity() const {
		return m_Impl->GetLocalIdentity();
	}

	bool GnsTransport::IsCompiledIn() {
#ifdef CCCP_WITH_GNS
		return true;
#else
		return false;
#endif
	}

	void GnsTransport::SetSimulatedLagMs(int lagMs) {
#ifdef CCCP_WITH_GNS
		s_SimulatedLagMs = lagMs;
#else
		(void)lagMs;
#endif
	}

} // namespace RTE
