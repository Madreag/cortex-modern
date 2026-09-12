#pragma once

#include "NetTransport.h"

#include <cstdint>
#include <string>
#include <vector>

class ISteamNetworkingConnectionSignaling;
class ISteamNetworkingSignalingRecvContext;

namespace RTE {

	/// ICE settings for the P2P entry points (the GNS k_ESteamNetworkingConfig_P2P_* values).
	struct GnsP2PConfig {
		int iceEnable = 2; //!< k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_* bits; 2 = Private: RFC1918 host candidates only.
		std::string stunServerList; //!< Empty: no STUN server, so no reflexive candidate and no DNS lookup.
		int iceImplementation = 1; //!< 1 = the native ICE client; WebRTC (2) is not compiled into these builds.
		int rendezvousLogLevel = 0; //!< k_ESteamNetworkingConfig_LogLevel_P2PRendezvous; 0 keeps the GNS default.
		std::string localIdentity; //!< Non-empty: ResetIdentity to it first, which closes every GNS connection in the process.
		int localVirtualPort = -1; //!< The joiner's own virtual port; -1 uses the remote one.
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
		std::vector<std::string> config; //!< "Name=value" of each config value the P2P path sets, read back from the connection.
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

		/// P2P host: listens on a virtual port; connect requests arrive through ReceiveP2PSignal.
		bool StartHostP2P(int virtualPort, const GnsP2PConfig& config, std::string* error = nullptr);
		/// P2P joiner. Takes ownership of signaling: its Release() runs even when the connect fails.
		bool ConnectP2P(ISteamNetworkingConnectionSignaling* signaling, const std::string& peerIdentity, int remoteVirtualPort, const GnsP2PConfig& config, std::string* error = nullptr);
		/// Hands one rendezvous blob from the peer to GNS; context answers connect requests and rejections.
		bool ReceiveP2PSignal(const void* blob, int size, ISteamNetworkingSignalingRecvContext* context);
		GnsPeerConnectionInfo GetPeerConnectionInfo(NetPeerId peerId) const;
		std::string GetPeerDetailedStatus(NetPeerId peerId) const;
		/// The GNS identity of this process; every transport in it shares one.
		std::string GetLocalIdentity() const;

		static bool IsCompiledIn();

		/// Test harness: adds a simulated round-trip lag (ms) to every connection made after the call.
		static void SetSimulatedLagMs(int lagMs);

	private:
		struct Impl;
		Impl* m_Impl = nullptr;
	};

} // namespace RTE
