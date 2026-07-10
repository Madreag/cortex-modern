#include "NetAuthSelfTest.h"

#include "NetAuthCrypto.h"
#include "NetSeatAuth.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace RTE {

	namespace {
		int Fail(const std::string& message) {
			std::cerr << "[net-auth-selftest] FAIL: " << message << std::endl;
			return 1;
		}

		std::vector<uint8_t> HexToBytes(const std::string& hex) {
			std::vector<uint8_t> bytes;
			for (size_t i = 0; i + 1 < hex.size(); i += 2) {
				bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
			}
			return bytes;
		}

		std::vector<uint8_t> RepeatBytes(uint8_t value, size_t count) {
			return std::vector<uint8_t>(count, value);
		}

		std::vector<uint8_t> TextBytes(const std::string& text) {
			return std::vector<uint8_t>(text.begin(), text.end());
		}

		// Deterministic test-only provider; installed only through SetNetAuthCryptoForTest, never
		// reachable from runtime or network input. The mac is a plain fold, NOT cryptographic.
		class DeterministicAuthCrypto : public NetAuthCrypto {
		public:
			bool failRandom = false;

			bool IsRealCrypto() const override { return false; }

			bool RandomBytes(uint8_t* buffer, size_t count) override {
				if (failRandom || buffer == nullptr) {
					return false;
				}
				for (size_t i = 0; i < count; ++i) {
					buffer[i] = m_Counter++;
					if (m_Counter == 0) {
						m_Counter = 1;
					}
				}
				return true;
			}

			bool HmacSha256(const uint8_t* key, size_t keyCount, const uint8_t* message, size_t messageCount, uint8_t (&mac)[32]) override {
				if (key == nullptr || keyCount == 0) {
					return false;
				}
				uint64_t fold = 1469598103934665603ull;
				const auto mix = [&fold](uint8_t byte) { fold = (fold ^ byte) * 1099511628211ull; };
				for (size_t i = 0; i < keyCount; ++i) {
					mix(key[i]);
				}
				mix(static_cast<uint8_t>(messageCount));
				for (size_t i = 0; i < messageCount; ++i) {
					mix(message[i]);
				}
				for (size_t i = 0; i < sizeof(mac); ++i) {
					mix(static_cast<uint8_t>(i));
					mac[i] = static_cast<uint8_t>(fold >> 32);
				}
				return true;
			}

		private:
			uint8_t m_Counter = 1;
		};

		struct ScopedTestCrypto {
			explicit ScopedTestCrypto(NetAuthCrypto* provider) { SetNetAuthCryptoForTest(provider); }
			~ScopedTestCrypto() { SetNetAuthCryptoForTest(nullptr); }
		};

		struct HmacVector {
			const char* name;
			std::vector<uint8_t> key;
			std::vector<uint8_t> message;
			std::string mac;
		};

		// RFC 4231 HMAC-SHA-256 test cases 1-4, 6, 7 (5 is truncated output, unused here).
		std::vector<HmacVector> MakeHmacVectors() {
			std::vector<HmacVector> vectors;
			vectors.push_back({"rfc4231-1", RepeatBytes(0x0b, 20), TextBytes("Hi There"), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"});
			vectors.push_back({"rfc4231-2", TextBytes("Jefe"), TextBytes("what do ya want for nothing?"), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"});
			vectors.push_back({"rfc4231-3", RepeatBytes(0xaa, 20), RepeatBytes(0xdd, 50), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"});
			vectors.push_back({"rfc4231-4", HexToBytes("0102030405060708090a0b0c0d0e0f10111213141516171819"), RepeatBytes(0xcd, 50), "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"});
			vectors.push_back({"rfc4231-6", RepeatBytes(0xaa, 131), TextBytes("Test Using Larger Than Block-Size Key - Hash Key First"), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"});
			vectors.push_back({"rfc4231-7", RepeatBytes(0xaa, 131), TextBytes("This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm."), "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"});
			return vectors;
		}
	} // namespace

	int NetAuthSelfTest::Run() {
		const uint8_t left[4] = {1, 2, 3, 4};
		const uint8_t right[4] = {1, 2, 3, 4};
		const uint8_t firstOff[4] = {0, 2, 3, 4};
		const uint8_t lastOff[4] = {1, 2, 3, 5};
		if (!NetAuthConstantTimeEquals(left, right, sizeof(left))) {
			return Fail("constant-time equals rejected equal buffers");
		}
		if (NetAuthConstantTimeEquals(left, firstOff, sizeof(left)) || NetAuthConstantTimeEquals(left, lastOff, sizeof(left))) {
			return Fail("constant-time equals accepted unequal buffers");
		}
		if (NetAuthConstantTimeEquals(nullptr, right, sizeof(right))) {
			return Fail("constant-time equals accepted a null buffer");
		}

#ifdef CCCP_WITH_GNS
		NetAuthCrypto& provider = GetNetAuthCrypto();
		if (!provider.IsRealCrypto()) {
			return Fail("GNS build default provider must be real crypto");
		}
		for (const HmacVector& vector: MakeHmacVectors()) {
			uint8_t mac[32] = {};
			if (!provider.HmacSha256(vector.key.data(), vector.key.size(), vector.message.data(), vector.message.size(), mac)) {
				return Fail(std::string("hmac refused vector ") + vector.name);
			}
			const std::vector<uint8_t> expected = HexToBytes(vector.mac);
			if (expected.size() != sizeof(mac) || std::memcmp(mac, expected.data(), sizeof(mac)) != 0) {
				return Fail(std::string("hmac known-answer mismatch on ") + vector.name);
			}
		}
		uint8_t drawA[32] = {};
		uint8_t drawB[32] = {};
		if (!provider.RandomBytes(drawA, sizeof(drawA)) || !provider.RandomBytes(drawB, sizeof(drawB))) {
			return Fail("csprng refused to draw");
		}
		if (std::memcmp(drawA, drawB, sizeof(drawA)) == 0) {
			return Fail("csprng drew identical buffers");
		}
		const uint8_t zero[32] = {};
		if (std::memcmp(drawA, zero, sizeof(drawA)) == 0) {
			return Fail("csprng drew all zeros");
		}
		uint8_t rejectMac[32] = {};
		if (provider.HmacSha256(nullptr, 0, drawA, sizeof(drawA), rejectMac)) {
			return Fail("hmac accepted an empty key");
		}
#else
		if (GetNetAuthCrypto().IsRealCrypto()) {
			return Fail("GNS-less default provider must fail closed");
		}
		uint8_t buffer[16] = {};
		if (GetNetAuthCrypto().RandomBytes(buffer, sizeof(buffer))) {
			return Fail("fail-closed provider drew random bytes");
		}
		uint8_t mac[32] = {};
		const uint8_t key[4] = {1, 2, 3, 4};
		if (GetNetAuthCrypto().HmacSha256(key, sizeof(key), buffer, sizeof(buffer), mac)) {
			return Fail("fail-closed provider produced a mac");
		}
		NetSeatAuthRegistry closedRegistry;
		if (closedRegistry.BeginHostedSession() || closedRegistry.IsActive()) {
			return Fail("registry armed without real crypto");
		}
#endif

		{
			DeterministicAuthCrypto deterministic;
			deterministic.failRandom = true;
			ScopedTestCrypto scoped(&deterministic);
			NetSeatAuthRegistry registry;
			if (registry.BeginHostedSession() || registry.IsActive()) {
				return Fail("registry armed under a failing csprng");
			}
		}

		{
			DeterministicAuthCrypto deterministic;
			ScopedTestCrypto scoped(&deterministic);
			NetSeatAuthRegistry registry;
			if (!registry.BeginHostedSession() || !registry.IsActive()) {
				return Fail("registry refused to arm on the test provider");
			}
			const NetAuthEpoch firstEpoch = registry.GetEpoch();
			const NetAuthEpoch zeroEpoch{};
			if (firstEpoch == zeroEpoch) {
				return Fail("armed epoch was all zeros");
			}
			if (!registry.BeginHostedSession() || registry.GetEpoch() == firstEpoch) {
				return Fail("re-arming did not draw a fresh epoch");
			}

			uint32_t generation = 0;
			NetSeatCredential credential{};
			if (!registry.IssueCredential(3, generation, credential) || generation != 1) {
				return Fail("first issuance did not open generation 1");
			}
			if (!registry.MatchesActiveCredential(3, 1, credential)) {
				return Fail("issued credential did not verify");
			}
			if (registry.MatchesActiveCredential(3, 2, credential) || registry.MatchesActiveCredential(3, 0, credential)) {
				return Fail("wrong generation verified");
			}
			NetSeatCredential tampered = credential;
			tampered[31] = static_cast<uint8_t>(tampered[31] ^ 0x01);
			if (registry.MatchesActiveCredential(3, 1, tampered)) {
				return Fail("tampered credential verified");
			}
			if (registry.MatchesActiveCredential(4, 1, credential)) {
				return Fail("unknown seat verified");
			}
			if (registry.GetActiveGeneration(4) != 0) {
				return Fail("unknown seat reported a generation");
			}

			// Mid-session RNG failure: issuance refuses and the seat's prior state stands.
			deterministic.failRandom = true;
			uint32_t failedGeneration = 0;
			NetSeatCredential failedCredential{};
			if (registry.IssueCredential(3, failedGeneration, failedCredential)) {
				return Fail("issuance succeeded under a failing csprng");
			}
			if (!registry.MatchesActiveCredential(3, 1, credential) || registry.GetActiveGeneration(3) != 1) {
				return Fail("failed issuance disturbed the active credential");
			}
			deterministic.failRandom = false;

			// Substitution: a new generation invalidates the prior one.
			uint32_t secondGeneration = 0;
			NetSeatCredential secondCredential{};
			if (!registry.IssueCredential(3, secondGeneration, secondCredential) || secondGeneration != 2) {
				return Fail("reissue did not advance the generation");
			}
			if (registry.MatchesActiveCredential(3, 1, credential)) {
				return Fail("prior generation survived a reissue");
			}
			if (!registry.MatchesActiveCredential(3, 2, secondCredential)) {
				return Fail("reissued credential did not verify");
			}

			// Clean leave: revocation kills the credential; generations never rewind.
			registry.RevokeSeat(3);
			if (registry.MatchesActiveCredential(3, 2, secondCredential) || registry.GetActiveGeneration(3) != 0) {
				return Fail("revoked credential verified");
			}
			uint32_t thirdGeneration = 0;
			NetSeatCredential thirdCredential{};
			if (!registry.IssueCredential(3, thirdGeneration, thirdCredential) || thirdGeneration != 3) {
				return Fail("post-revoke issuance rewound the generation");
			}

			registry.EndSession();
			if (registry.IsActive() || registry.GetEpoch() != zeroEpoch) {
				return Fail("session end left auth material armed");
			}
			if (registry.MatchesActiveCredential(3, 3, thirdCredential)) {
				return Fail("credential survived session end");
			}
		}

		if (GetNetAuthCrypto().IsRealCrypto() !=
#ifdef CCCP_WITH_GNS
		    true
#else
		    false
#endif
		) {
			return Fail("test override leaked past its scope");
		}

		std::cout << "[net-auth-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
