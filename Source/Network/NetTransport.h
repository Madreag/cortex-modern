#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	using NetPeerId = uint32_t;

	constexpr NetPeerId c_InvalidNetPeerId = 0;

	enum class NetTransportLane : uint8_t {
		ControlReliable = 0,
		InputUnreliable = 1,
		DiagnosticsReliable = 2,
	};

	enum class NetTransportEventType {
		PeerConnected,
		PeerDisconnected,
		PacketReceived,
		ConnectionFailed,
		TransportError,
	};

	struct NetTransportEvent {
		NetTransportEventType type = NetTransportEventType::TransportError;
		NetPeerId peerId = c_InvalidNetPeerId;
		NetTransportLane lane = NetTransportLane::ControlReliable;
		std::vector<uint8_t> bytes;
		std::string reason;
	};

	class INetTransport {
	public:
		virtual ~INetTransport() = default;

		virtual bool StartHost(uint16_t port, std::string* error = nullptr) = 0;
		virtual bool Connect(const std::string& address, uint16_t port, std::string* error = nullptr) = 0;
		virtual bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr) = 0;
		virtual void Disconnect(NetPeerId peerId, const std::string& reason) = 0;
		virtual void Stop() = 0;
		virtual std::vector<NetTransportEvent> PollEvents() = 0;
	};

} // namespace RTE
