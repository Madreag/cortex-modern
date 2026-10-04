#pragma once

#include "NetTransport.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class ISteamNetworkingConnectionSignaling;
class ISteamNetworkingSignalingRecvContext;

namespace RTE {

	/// How long an ICE connect may take to reach Connected: a relayed connect whose candidates cross a slow signalling path
	/// outlasts GNS's 10 s default, while a dead session still fails inside it.
	constexpr uint32_t c_IceConnectTimeoutMs = 30000;

	/// The route each connection last named in a receipt; a connection whose live route differs has moved.
	class GnsRouteTracker {
	public:
		enum class Observation { First, Same, Moved };
		Observation Observe(uint64_t connection, bool relayed) {
			const auto [entry, inserted] = m_Relayed.try_emplace(connection, relayed);
			if (inserted) return Observation::First;
			if (entry->second == relayed) return Observation::Same;
			entry->second = relayed;
			return Observation::Moved;
		}
		void Forget(uint64_t connection) { m_Relayed.erase(connection); }
		void Clear() { m_Relayed.clear(); }
		/// The move a Moved observation of relayed reports.
		static const char* MoveName(bool relayed) { return relayed ? "direct->relay" : "relay->direct"; }

	private:
		std::map<uint64_t, bool> m_Relayed;
	};

	/// ICE settings for the P2P entry points (the GNS k_ESteamNetworkingConfig_P2P_* values).
	struct GnsP2PConfig {
		int iceEnable = 2; //!< k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_* bits; 2 = Private: RFC1918 host candidates only.
		std::string stunServerList; //!< Empty: no STUN server, so no reflexive candidate and no DNS lookup.
		std::string turnServerList;
		std::string turnUserList;
		std::string turnPassList;
		int connectionMode = 0; //!< Automatic, direct only, relay only.
		int iceImplementation = 1; //!< 1 = the native ICE client; WebRTC (2) is not compiled into these builds.
		int rendezvousLogLevel = 0; //!< k_ESteamNetworkingConfig_LogLevel_P2PRendezvous; 0 keeps the GNS default.
		std::string localIdentity; //!< Non-empty: ResetIdentity to it first, which closes every GNS connection in the process.
		int localVirtualPort = -1; //!< The joiner's own virtual port; -1 uses the remote one.
		std::string relayOffer = "none"; //!< The relay offer the TURN lists came from (its match id and expiry), named on each relayed route.
	};

	/// A peer connection as GNS reports it: GetConnectionInfo plus the P2P config the connection runs with.
	struct GnsPeerConnectionInfo {
		bool found = false;
		int state = 0;
		int endReason = 0;
		std::string endDebug;
		std::string description;
		std::string remoteIdentity;
		std::string remoteAddress; //!< For ICE, the remote candidate in use; GNS clears it for a relayed route.
		int flags = 0;
		uint32_t relayPop = 0;
		std::string connectedRoute;
		std::string selectedCandidateType;
		std::string routeReceipt; //!< The connection's [net-route] line, once its route is chosen.
		std::vector<std::string> config; //!< "Name=value" of each config value the P2P path sets, read back from the connection.
		NetPeerId peerId = c_InvalidNetPeerId; //!< The transport's own id for the peer.
		std::string relayOffer = "none"; //!< The relay offer the route line names: the connection's for a relayed route, none for a direct one.
	};

	class GnsTransport;

	/// A live connection of one of the process's transports.
	struct GnsProcessConnection {
		const GnsTransport* transport = nullptr;
		bool p2p = false; //!< The transport runs ICE rather than direct IP.
		bool host = false; //!< The transport listens, so the connection was accepted rather than dialed.
		GnsPeerConnectionInfo info;
	};

	/// What the fake link did to the packets an end received: GNS's own counters on that end.
	struct NetFakeLinkEffects {
		int64_t jitterPackets = 0;     ///< Packets that arrived 1 ms or more off their link's steady latency.
		int64_t reorderedPackets = 0;
		int64_t duplicatedPackets = 0;
	};

	class GnsTransport : public INetTransport {
	public:
		GnsTransport();
		~GnsTransport() override;

		GnsTransport(const GnsTransport&) = delete;
		GnsTransport& operator=(const GnsTransport&) = delete;

		bool StartHost(uint16_t port, std::string* error = nullptr) override;
		bool Connect(const std::string& address, uint16_t port, std::string* error = nullptr) override;
		bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override;
		void Disconnect(NetPeerId peerId, const std::string& reason) override;
		void Stop() override;
		std::vector<NetTransportEvent> PollEvents() override;
		uint32_t GetPeerPingMs(NetPeerId peerId) const override;
		std::string GetConnectedRoute(NetPeerId peerId) const override { return GetPeerConnectionInfo(peerId).connectedRoute; }

		/// P2P host: listens on a virtual port; connect requests arrive through ReceiveP2PSignal.
		bool StartHostP2P(int virtualPort, const GnsP2PConfig& config, std::string* error = nullptr);
		/// P2P joiner. Takes ownership of signaling: its Release() runs even when the connect fails.
		bool ConnectP2P(ISteamNetworkingConnectionSignaling* signaling, const std::string& peerIdentity, int remoteVirtualPort, const GnsP2PConfig& config, std::string* error = nullptr);
		/// Hands one rendezvous blob from the peer to GNS; context answers connect requests and rejections.
		bool ReceiveP2PSignal(const void* blob, int size, ISteamNetworkingSignalingRecvContext* context);
		GnsPeerConnectionInfo GetPeerConnectionInfo(NetPeerId peerId) const;
		/// Every live connection of every transport in this process, each transport's in peer order.
		static std::vector<GnsProcessConnection> GetProcessConnections();
		std::string GetPeerDetailedStatus(NetPeerId peerId) const;
		/// The fake link's effects on what this transport's connections received, summed.
		NetFakeLinkEffects GetFakeLinkEffects() const;
		/// One connection's effects, read from its detailed status: its own end's lifetime counters, not the remote host's.
		static NetFakeLinkEffects ParseFakeLinkEffects(const std::string& detailedStatus);
		/// The fake link's requested jitter, reorder and duplicate settings; zeros while it is off.
		static void GetFakeLinkSettings(int& jitterMs, float& reorderPercent, float& duplicatePercent);
		/// The GNS identity of this process; every transport in it shares one.
		std::string GetLocalIdentity() const;
		/// The same identity read without a transport of its own; empty while GNS is not running in this process.
		static std::string ProcessIdentity();
		/// Updates the credentials used by subsequent ICE connections on this listener.
		static void ApplyIceServers(const GnsP2PConfig& config);
		/// Also hands a changed relay login to the TURN allocations of the live P2P connections.
		void UpdateListenerIceServers(const GnsP2PConfig& config);
		static bool ConnectionPolicyAllowsRoute(int mode, bool relayed) { return mode == 1 ? !relayed : mode != 2 || relayed; }
		/// The connect limit ICE connections run with: c_IceConnectTimeoutMs, or CC_TEST_ICE_CONNECT_TIMEOUT_MS when a measurement sets it.
		static uint32_t IceConnectTimeoutMs();

		static bool IsCompiledIn();

		/// Test harness: adds a simulated round-trip lag (ms) to every connection made after the call.
		static void SetSimulatedLagMs(int lagMs);
		/// Test harness: the jitter every connection adds on top of the simulated lag, as a cross fault's jitter_ms does.
		static void SetSimulatedJitterMs(int jitterMs);
		/// Test harness: the share of every connection's packets reordered (20 ms late) and duplicated (up to 20 ms after), each way.
		static void SetSimulatedReorderPercent(float percent);
		static void SetSimulatedDuplicatePercent(float percent);

		/// Diagnostics: prints GNS's own rendezvous and ICE spew at this debug level (0 = off).
		static void SetRendezvousLogLevel(int level);
		/// Exercises the pre-announcement payload queue without opening a socket.
		static bool PayloadHoldSelfTest(std::string* error = nullptr);

	private:
		struct Impl;
		Impl* m_Impl = nullptr;
	};

} // namespace RTE
