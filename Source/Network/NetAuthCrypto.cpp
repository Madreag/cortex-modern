#include "NetAuthCrypto.h"

#include <limits>

#ifdef CCCP_WITH_GNS
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace RTE {

	namespace {
#ifdef CCCP_WITH_GNS
		class OpenSslAuthCrypto : public NetAuthCrypto {
		public:
			bool IsRealCrypto() const override { return true; }

			bool RandomBytes(uint8_t* buffer, size_t count) override {
				if (buffer == nullptr || count == 0 || count > static_cast<size_t>(std::numeric_limits<int>::max())) {
					return false;
				}
				return RAND_bytes(buffer, static_cast<int>(count)) == 1;
			}

			bool HmacSha256(const uint8_t* key, size_t keyCount, const uint8_t* message, size_t messageCount, uint8_t (&mac)[32]) override {
				if (key == nullptr || keyCount == 0 || (message == nullptr && messageCount > 0)) {
					return false;
				}
				size_t macCount = 0;
				return EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key, keyCount, message, messageCount, mac, sizeof(mac), &macCount) != nullptr && macCount == sizeof(mac);
			}
		};
		using DefaultAuthCrypto = OpenSslAuthCrypto;
#else
		// GNS-less builds carry no crypto library; everything refuses so no auth material can exist.
		class FailClosedAuthCrypto : public NetAuthCrypto {
		public:
			bool IsRealCrypto() const override { return false; }
			bool RandomBytes(uint8_t*, size_t) override { return false; }
			bool HmacSha256(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t (&)[32]) override { return false; }
		};
		using DefaultAuthCrypto = FailClosedAuthCrypto;
#endif

		NetAuthCrypto* s_TestOverride = nullptr;
	} // namespace

	NetAuthCrypto& GetNetAuthCrypto() {
		static DefaultAuthCrypto defaultProvider;
		return s_TestOverride != nullptr ? *s_TestOverride : defaultProvider;
	}

	void SetNetAuthCryptoForTest(NetAuthCrypto* provider) {
		s_TestOverride = provider;
	}

	bool NetAuthConstantTimeEquals(const uint8_t* a, const uint8_t* b, size_t count) {
		if (a == nullptr || b == nullptr) {
			return false;
		}
		uint8_t difference = 0;
		for (size_t i = 0; i < count; ++i) {
			difference = static_cast<uint8_t>(difference | (a[i] ^ b[i]));
		}
		return difference == 0;
	}

} // namespace RTE
