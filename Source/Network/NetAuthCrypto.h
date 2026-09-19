#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

namespace RTE {

	/// Injectable crypto for the reconnect-auth layer. The default provider is OpenSSL on GNS
	/// builds and fails closed (every operation refuses) when GNS is absent.
	class NetAuthCrypto {
	public:
		virtual ~NetAuthCrypto() = default;

		/// Whether this provider is backed by a real CSPRNG and HMAC.
		virtual bool IsRealCrypto() const = 0;

		/// Fills the buffer from the CSPRNG.
		/// @return Whether the buffer was filled; false means no auth material may be issued.
		virtual bool RandomBytes(uint8_t* buffer, size_t count) = 0;

		/// Computes HMAC-SHA-256 of the message under the key.
		/// @return Whether the mac was produced.
		virtual bool HmacSha256(const uint8_t* key, size_t keyCount, const uint8_t* message, size_t messageCount, uint8_t (&mac)[32]) = 0;
	};

	/// The active provider: the test override when one is installed, the build's default otherwise.
	NetAuthCrypto& GetNetAuthCrypto();

	/// Test-only override, reachable only from selftest code; nullptr restores the default.
	void SetNetAuthCryptoForTest(NetAuthCrypto* provider);

	/// Constant-time equality over count bytes of both buffers.
	bool NetAuthConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t count);
	bool NetAuthSeal(const std::array<uint8_t, 32>& key, const std::vector<uint8_t>& context, const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& sealed);
	bool NetAuthOpen(const std::array<uint8_t, 32>& key, const std::vector<uint8_t>& context, const std::vector<uint8_t>& sealed, std::vector<uint8_t>& plaintext);

} // namespace RTE
