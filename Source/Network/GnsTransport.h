#pragma once

#include "NetTransport.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	class GnsTransport : public INetTransport {
	public:
		GnsTransport();
		~GnsTransport() override;

		GnsTransport(const GnsTransport&) = delete;
		GnsTransport& operator=(const GnsTransport&) = delete;

		bool StartHost(uint16_t port, std::string* error = nullptr) override;
		bool Connect(const std::string& address, uint16_t port, std::string* error = nullptr) override;
		bool Send(NetPeerId peerId, NetTransportLane lane, const std::vector<uint8_t>& bytes, std::string* error = nullptr, bool* congested = nullptr) override;
		void SetBulkTransferMode(bool on) override;
		void Disconnect(NetPeerId peerId, const std::string& reason) override;
		void Stop() override;
		std::vector<NetTransportEvent> PollEvents() override;
		uint32_t GetPeerPingMs(NetPeerId peerId) const override;

		static bool IsCompiledIn();

		/// Test harness: adds a simulated round-trip lag (ms) to every connection made after the call.
		static void SetSimulatedLagMs(int lagMs);

	private:
		struct Impl;
		Impl* m_Impl = nullptr;
	};

} // namespace RTE
