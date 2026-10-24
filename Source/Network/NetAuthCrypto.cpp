#include "NetAuthCrypto.h"

#include <limits>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>

#ifdef CCCP_WITH_GNS
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#endif

namespace RTE {

	namespace {
#ifdef CCCP_WITH_GNS
		// Outside the provider so its thread-local slots can use these member initializers inside the class.
		struct KeyedHmac {
			std::array<uint8_t, 64> key{};
			size_t keyCount = 0;
			std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> context{nullptr, EVP_MAC_CTX_free};
			~KeyedHmac() { OPENSSL_cleanse(key.data(), key.size()); }
		};

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
				// Fetching the algorithm by name takes the provider store's lock and a lookup, and keying hashes two pad blocks: each
				// thread keeps contexts for the keys it used last, so a message under one of them costs only its own blocks.
				static EVP_MAC* const hmac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
				if (hmac == nullptr) return false;
				KeyedHmac* slot = nullptr;
				for (auto& candidate: t_Keyed)
					if (candidate.context && candidate.keyCount == keyCount && std::equal(key, key + keyCount, candidate.key.begin())) slot = &candidate;
				const bool reused = slot != nullptr;
				if (!reused) {
					slot = &t_Keyed[t_NextKeyed];
					t_NextKeyed = (t_NextKeyed + 1) % t_Keyed.size();
					OPENSSL_cleanse(slot->key.data(), slot->key.size());
					slot->keyCount = 0;
					if (!slot->context) {
						std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> fresh{EVP_MAC_CTX_new(hmac), EVP_MAC_CTX_free};
						char digest[] = "SHA256";
						const OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0), OSSL_PARAM_construct_end()};
						if (!fresh || EVP_MAC_CTX_set_params(fresh.get(), params) != 1) return false;
						slot->context = std::move(fresh);
					}
				}
				size_t macCount = 0;
				const bool keyed = reused ? EVP_MAC_init(slot->context.get(), nullptr, 0, nullptr) == 1 : EVP_MAC_init(slot->context.get(), key, keyCount, nullptr) == 1;
				// A key longer than a slot is keyed again for every message.
				if (keyed && !reused && keyCount <= slot->key.size()) {
					std::copy_n(key, keyCount, slot->key.begin());
					slot->keyCount = keyCount;
				}
				return keyed && EVP_MAC_update(slot->context.get(), message, messageCount) == 1 && EVP_MAC_final(slot->context.get(), mac, &macCount, sizeof(mac)) == 1 &&
				    macCount == sizeof(mac);
			}

		private:
			static inline thread_local std::array<KeyedHmac, 8> t_Keyed;
			static inline thread_local size_t t_NextKeyed = 0;
		};
#endif
		// GNS-less builds carry no crypto library; everything refuses so no auth material can exist.
		class FailClosedAuthCrypto : public NetAuthCrypto {
		public:
			bool IsRealCrypto() const override { return false; }
			bool RandomBytes(uint8_t*, size_t) override { return false; }
			bool HmacSha256(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t (&)[32]) override { return false; }
		};
#ifdef CCCP_WITH_GNS
		using DefaultAuthCrypto = OpenSslAuthCrypto;
#else
		using DefaultAuthCrypto = FailClosedAuthCrypto;
#endif

		NetAuthCrypto* s_TestOverride = nullptr;
	} // namespace

	NetAuthCrypto& GetNetAuthCrypto() {
		static DefaultAuthCrypto defaultProvider;
		// Test lever: CCCP_TEST_FAIL_CLOSED_CRYPTO stands this build in for one without its cryptography. Unset, nothing changes.
		static FailClosedAuthCrypto failClosedProvider;
		static const bool failClosed = std::getenv("CCCP_TEST_FAIL_CLOSED_CRYPTO") != nullptr;
		return s_TestOverride != nullptr ? *s_TestOverride : failClosed ? static_cast<NetAuthCrypto&>(failClosedProvider) : defaultProvider;
	}

	bool NetAuthSeal(const std::array<uint8_t, 32>& key, const std::vector<uint8_t>& context, const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& sealed) {
		sealed.clear();
#ifdef CCCP_WITH_GNS
		if (plaintext.empty() || plaintext.size() > 48 * 1024 || context.size() > 1024)
			return false;
		std::vector<uint8_t> result(12 + plaintext.size() + 16);
		if (!GetNetAuthCrypto().RandomBytes(result.data(), 12))
			return false;
		std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
		int size = 0, tail = 0;
		if (!cipher || EVP_EncryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, key.data(), result.data()) != 1 ||
		    EVP_EncryptUpdate(cipher.get(), nullptr, &size, context.data(), static_cast<int>(context.size())) != 1 ||
		    EVP_EncryptUpdate(cipher.get(), result.data() + 12, &size, plaintext.data(), static_cast<int>(plaintext.size())) != 1 ||
		    EVP_EncryptFinal_ex(cipher.get(), result.data() + 12 + size, &tail) != 1 || static_cast<size_t>(size + tail) != plaintext.size() ||
		    EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16, result.data() + 12 + plaintext.size()) != 1)
			return false;
		sealed = std::move(result);
		return true;
#else
		return false;
#endif
	}

	bool NetAuthOpen(const std::array<uint8_t, 32>& key, const std::vector<uint8_t>& context, const std::vector<uint8_t>& sealed, std::vector<uint8_t>& plaintext) {
		plaintext.clear();
#ifdef CCCP_WITH_GNS
		if (sealed.size() <= 28 || sealed.size() > 48 * 1024 + 28 || context.size() > 1024)
			return false;
		const size_t body = sealed.size() - 28;
		std::vector<uint8_t> result(body);
		std::array<uint8_t, 16> tag{};
		std::copy_n(sealed.end() - 16, 16, tag.begin());
		std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
		int size = 0, tail = 0;
		if (!cipher || EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, key.data(), sealed.data()) != 1 ||
		    EVP_DecryptUpdate(cipher.get(), nullptr, &size, context.data(), static_cast<int>(context.size())) != 1 ||
		    EVP_DecryptUpdate(cipher.get(), result.data(), &size, sealed.data() + 12, static_cast<int>(body)) != 1 ||
		    EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1 ||
		    EVP_DecryptFinal_ex(cipher.get(), result.data() + size, &tail) != 1 || static_cast<size_t>(size + tail) != body) {
			std::fill(result.begin(), result.end(), 0);
			return false;
		}
		plaintext = std::move(result);
		return true;
#else
		return false;
#endif
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
