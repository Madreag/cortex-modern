#include "NetReconnectSelfTest.h"

#include "NetAuthCrypto.h"
#include "NetReconnectAdmission.h"
#include "NetReconnectTranscript.h"
#include "NetReconnectTxCache.h"
#include "NetSeatAuth.h"

#include <array>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

	namespace {
		int Fail(const std::string& message) {
			std::cerr << "[net-reconnect-selftest] FAIL: " << message << std::endl;
			return 1;
		}

		template <size_t N>
		std::array<uint8_t, N> Ramp(uint8_t seed) {
			std::array<uint8_t, N> bytes{};
			for (size_t i = 0; i < bytes.size(); ++i) {
				bytes[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
			}
			return bytes;
		}

		std::string HexOf(const uint8_t* bytes, size_t count, bool upper) {
			static const char* lowerDigits = "0123456789abcdef";
			static const char* upperDigits = "0123456789ABCDEF";
			const char* digits = upper ? upperDigits : lowerDigits;
			std::string text;
			text.reserve(count * 2);
			for (size_t i = 0; i < count; ++i) {
				text.push_back(digits[bytes[i] >> 4]);
				text.push_back(digits[bytes[i] & 0x0FU]);
			}
			return text;
		}

		// Deterministic test-only provider; installed only through SetNetAuthCryptoForTest. The mac is
		// a plain fold, NOT cryptographic - the real HMAC is covered by the known-answer vectors.
		class ScriptedAuthCrypto : public NetAuthCrypto {
		public:
			bool failRandom = false;
			bool failHmac = false;

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
				if (failHmac || key == nullptr || keyCount == 0) {
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

		// Redirects the engine's own console streams so the canary scan reads exactly what a player
		// or a crash artifact would see.
		class ScopedConsoleCapture {
		public:
			ScopedConsoleCapture() {
				m_OutBuffer = std::cout.rdbuf(m_Captured.rdbuf());
				m_ErrBuffer = std::cerr.rdbuf(m_Captured.rdbuf());
			}
			~ScopedConsoleCapture() {
				std::cout.rdbuf(m_OutBuffer);
				std::cerr.rdbuf(m_ErrBuffer);
			}
			std::string Text() const { return m_Captured.str(); }

		private:
			std::ostringstream m_Captured;
			std::streambuf* m_OutBuffer = nullptr;
			std::streambuf* m_ErrBuffer = nullptr;
		};

		NetH4Transcript MakeTranscript() {
			NetH4Transcript transcript;
			transcript.domain = NetH4ProofDomain::Reclaim;
			transcript.protocolVersion = NetProtocol::c_Version;
			transcript.epoch = Ramp<16>(0x10);
			transcript.stableSeat = 2;
			transcript.holderGeneration = 7;
			transcript.challenge = Ramp<32>(0x20);
			transcript.clientNonce = Ramp<16>(0x40);
			return transcript;
		}

		NetH4Identity MakeIdentity() {
			NetH4Identity identity;
			identity.controllerFrameVersion = 5;
			identity.controllerFrameEncodedSize = 80;
			identity.gameVersion = "7.0.0";
			identity.buildId = "stage2-h4a";
			identity.deterministicConfigHash = Ramp<32>(1);
			identity.moduleManifestHash = Ramp<32>(33);
			identity.sessionRulesHash = Ramp<32>(65);
			identity.sessionIdentityHash = Ramp<32>(97);
			return identity;
		}

		int TestTranscriptLayout() {
			const std::vector<std::pair<NetH4ProofDomain, std::string>> tags = {
				{NetH4ProofDomain::Reclaim, std::string("CCCP.H4.RECLAIM\0", 16)},
				{NetH4ProofDomain::Substitution, std::string("CCCP.H4.SUBST\0\0\0", 16)},
				{NetH4ProofDomain::Ticket, std::string("CCCP.H4.TICKET\0\0", 16)},
			};
			for (const auto& [domain, expected] : tags) {
				const char* tag = NetH4DomainTag(domain);
				if (tag == nullptr || std::memcmp(tag, expected.data(), c_NetH4DomainTagBytes) != 0) {
					return Fail("domain tag bytes differ for domain " + std::to_string(static_cast<int>(domain)));
				}
			}

			NetH4TranscriptBytes bytes{};
			if (!NetH4BuildTranscript(MakeTranscript(), bytes)) {
				return Fail("transcript refused a well-formed input");
			}
			const std::string expected =
			    "434343502e48342e5245434c41494d000100101112131415161718191a1b1c1d1e1f0200"
			    "07000000202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
			    "404142434445464748494a4b4c4d4e4f";
			if (HexOf(bytes.data(), bytes.size(), false) != expected) {
				return Fail("canonical transcript bytes differ: " + HexOf(bytes.data(), bytes.size(), false));
			}

			// A seat with no holder has no credential, so there is nothing to sign.
			NetH4Transcript unheld = MakeTranscript();
			unheld.holderGeneration = 0;
			if (NetH4BuildTranscript(unheld, bytes)) {
				return Fail("transcript accepted holder generation 0");
			}
			return 0;
		}

		int TestKnownAnswers() {
			const NetAuthBytes32 credential = Ramp<32>(0);
			const NetH4Transcript reclaim = MakeTranscript();
			NetAuthBytes32 mac{};
			if (!NetH4ComputeProof(credential, reclaim, mac)) {
				return Fail("proof refused under the real provider");
			}
			if (HexOf(mac.data(), mac.size(), false) != "f0ea2c3738f502c106cee167fd3ff79396050fc8444420d6f0c0ddaaf4e7b93d") {
				return Fail("reclaim proof known-answer mismatch: " + HexOf(mac.data(), mac.size(), false));
			}
			if (!NetH4VerifyProof(credential, reclaim, mac)) {
				return Fail("known-answer proof did not verify");
			}

			// Distinct tags, so a proof for one purpose is not a proof for another.
			NetH4Transcript substitution = reclaim;
			substitution.domain = NetH4ProofDomain::Substitution;
			NetAuthBytes32 substitutionMac{};
			if (!NetH4ComputeProof(credential, substitution, substitutionMac)) {
				return Fail("substitution proof refused");
			}
			if (HexOf(substitutionMac.data(), substitutionMac.size(), false) != "72add2add725f713e91b09be722e84e6a90e7810f4db3920a2189e071ee3ff09") {
				return Fail("substitution proof known-answer mismatch");
			}
			NetH4Transcript ticket = reclaim;
			ticket.domain = NetH4ProofDomain::Ticket;
			NetAuthBytes32 ticketMac{};
			if (!NetH4ComputeProof(credential, ticket, ticketMac) ||
			    HexOf(ticketMac.data(), ticketMac.size(), false) != "4f75c6ecab2aff41b967e468b5f9e3ced84958216a16a3830f8ff38f53cf7167") {
				return Fail("ticket proof known-answer mismatch");
			}
			if (NetH4VerifyProof(credential, substitution, mac) || NetH4VerifyProof(credential, reclaim, substitutionMac)) {
				return Fail("a proof verified across domains");
			}

			// Every bound field changes the proof, so a replay into another context is refused.
			std::vector<std::pair<std::string, NetH4Transcript>> altered;
			NetH4Transcript otherEpoch = reclaim;
			otherEpoch.epoch[0] = static_cast<uint8_t>(otherEpoch.epoch[0] ^ 0x01);
			altered.emplace_back("epoch", otherEpoch);
			NetH4Transcript otherSeat = reclaim;
			otherSeat.stableSeat = 3;
			altered.emplace_back("seat", otherSeat);
			NetH4Transcript otherGeneration = reclaim;
			otherGeneration.holderGeneration = 8;
			altered.emplace_back("generation", otherGeneration);
			NetH4Transcript otherChallenge = reclaim;
			otherChallenge.challenge[31] = static_cast<uint8_t>(otherChallenge.challenge[31] ^ 0x01);
			altered.emplace_back("challenge", otherChallenge);
			NetH4Transcript otherNonce = reclaim;
			otherNonce.clientNonce[15] = static_cast<uint8_t>(otherNonce.clientNonce[15] ^ 0x01);
			altered.emplace_back("nonce", otherNonce);
			NetH4Transcript otherVersion = reclaim;
			otherVersion.protocolVersion = static_cast<uint16_t>(reclaim.protocolVersion + 1);
			altered.emplace_back("protocol version", otherVersion);
			for (const auto& [name, transcript] : altered) {
				if (NetH4VerifyProof(credential, transcript, mac)) {
					return Fail("proof verified with a different " + name);
				}
			}

			NetAuthBytes32 wrongCredential = credential;
			wrongCredential[0] = static_cast<uint8_t>(wrongCredential[0] ^ 0x01);
			if (NetH4VerifyProof(wrongCredential, reclaim, mac)) {
				return Fail("proof verified under a different credential");
			}
			NetAuthBytes32 tampered = mac;
			tampered[31] = static_cast<uint8_t>(tampered[31] ^ 0x01);
			if (NetH4VerifyProof(credential, reclaim, tampered)) {
				return Fail("a tampered proof verified");
			}
			return 0;
		}

		int TestFailureInjection() {
			ScriptedAuthCrypto provider;
			ScopedTestCrypto scoped(&provider);
			const NetAuthBytes32 credential = Ramp<32>(0);
			const NetH4Transcript transcript = MakeTranscript();

			provider.failHmac = true;
			NetAuthBytes32 mac{};
			if (NetH4ComputeProof(credential, transcript, mac)) {
				return Fail("proof produced under a failing hmac");
			}
			if (NetH4VerifyProof(credential, transcript, mac)) {
				return Fail("verification passed under a failing hmac");
			}
			provider.failHmac = false;

			provider.failRandom = true;
			NetAuthBytes32 challenge{};
			NetAuthBytes16 nonce{};
			NetAuthBytes16 txId{};
			if (NetH4DrawChallenge(challenge) || NetH4DrawNonce(nonce) || NetH4DrawTxId(txId)) {
				return Fail("auth material was drawn under a failing csprng");
			}
			NetReconnectAdmission admission;
			if (!admission.BeginAttempt(1, 0)) {
				return Fail("first attempt was rate limited");
			}
			if (admission.IssueChallenge(1, txId, 2, 7, 0, challenge) || admission.IssueSyntheticChallenge(1, 0, challenge)) {
				return Fail("a challenge was issued under a failing csprng");
			}
			if (admission.GetRefusedChallenges() != 2 || admission.GetOutstandingChallengeCount() != 0) {
				return Fail("failed issuance was not counted or leaked a slot");
			}

			// The refusal is fail-closed but not silent: it takes the same uniform denial path.
			admission.ScheduleDenial(1, txId, NetH4DenialReason::ProviderUnavailable, 0);
			if (admission.ReleaseDueDenials(999).size() != 0 || admission.ReleaseDueDenials(1000).size() != 1) {
				return Fail("a provider-failure denial did not follow the uniform schedule");
			}
			return 0;
		}

		int TestTxCache() {
			NetReconnectTxCache cache;
			if (cache.GetMaxEntries() < 64 || cache.GetRetentionMs() != 60000) {
				return Fail("tx cache bounds are not the pinned floor and window");
			}
			const NetAuthBytes16 txId = Ramp<16>(0x10);
			NetH4TxKey key;
			key.requestType = NetMessageType::Reclaim;
			key.stableSeat = 2;
			key.holderGeneration = 7;
			key.identity = MakeIdentity();

			if (cache.Find(txId, key, 0) != nullptr) {
				return Fail("empty cache returned a result");
			}
			NetH4JoinCommitted committed;
			committed.txId = txId;
			committed.stableSeat = 2;
			committed.holderGeneration = 7;
			committed.incarnation = 1;
			committed.assignedPeerId = 2;
			cache.Store(txId, key, committed, 1000);

			// A duplicate valid proof replays the success it already earned.
			const NetPayload* replayed = cache.Find(txId, key, 1500);
			if (replayed == nullptr) {
				return Fail("a duplicate transaction did not replay its terminal result");
			}
			const auto* replayedCommit = std::get_if<NetH4JoinCommitted>(replayed);
			if (replayedCommit == nullptr || !(*replayedCommit == committed)) {
				return Fail("the replayed terminal result was not byte-identical");
			}

			// The id alone is not enough: it is drawn by the client, so the rest must match too.
			NetH4TxKey otherSeat = key;
			otherSeat.stableSeat = 3;
			NetH4TxKey otherGeneration = key;
			otherGeneration.holderGeneration = 8;
			NetH4TxKey otherType = key;
			otherType.requestType = NetMessageType::LeaveRequest;
			NetH4TxKey otherIdentity = key;
			otherIdentity.identity.moduleManifestHash[0] = static_cast<uint8_t>(otherIdentity.identity.moduleManifestHash[0] ^ 0x01);
			NetH4TxKey otherBuild = key;
			otherBuild.identity.buildId = "stage2-h4a-other";
			for (const NetH4TxKey& mismatched : {otherSeat, otherGeneration, otherType, otherIdentity, otherBuild}) {
				if (cache.Find(txId, mismatched, 1500) != nullptr) {
					return Fail("a re-presented transaction replayed under a different key");
				}
			}
			if (cache.GetKeyMismatches() != 5) {
				return Fail("key mismatches were not counted: " + std::to_string(cache.GetKeyMismatches()));
			}

			// Retention runs to the pinned window inclusive; a duplicate past it is a fresh denial.
			if (cache.Find(txId, key, 1000 + NetReconnectTxCache::c_RetentionMs) == nullptr) {
				return Fail("an entry expired inside the retention window");
			}
			if (cache.Find(txId, key, 1000 + NetReconnectTxCache::c_RetentionMs + 1) != nullptr) {
				return Fail("an entry survived past the retention window");
			}
			if (cache.Size() != 0 || cache.GetExpiredEvictions() != 1) {
				return Fail("expiry did not evict the entry");
			}

			// Full cache: expire by age first, then the oldest insertion.
			NetReconnectTxCache bounded(4, 60000);
			for (uint8_t i = 0; i < 4; ++i) {
				NetH4TxKey slot = key;
				slot.stableSeat = i;
				bounded.Store(Ramp<16>(static_cast<uint8_t>(i + 1)), slot, NetH4LeaveAck{c_NetH4Version, Ramp<16>(static_cast<uint8_t>(i + 1)), i, 7, true}, 100ULL + i);
			}
			if (bounded.Size() != 4) {
				return Fail("bounded cache did not hold its capacity");
			}
			NetH4TxKey fifth = key;
			fifth.stableSeat = 4;
			bounded.Store(Ramp<16>(5), fifth, NetH4LeaveAck{c_NetH4Version, Ramp<16>(5), 4, 7, true}, 200);
			NetH4TxKey first = key;
			first.stableSeat = 0;
			if (bounded.Size() != 4 || bounded.GetCapacityEvictions() != 1) {
				return Fail("the full cache did not evict exactly one entry");
			}
			if (bounded.Find(Ramp<16>(1), first, 200) != nullptr) {
				return Fail("the oldest insertion survived eviction");
			}
			for (uint8_t i = 1; i < 4; ++i) {
				NetH4TxKey slot = key;
				slot.stableSeat = i;
				if (bounded.Find(Ramp<16>(static_cast<uint8_t>(i + 1)), slot, 200) == nullptr) {
					return Fail("eviction removed a newer entry than the oldest");
				}
			}
			if (bounded.Find(Ramp<16>(5), fifth, 200) == nullptr) {
				return Fail("the newest entry was not stored");
			}

			// Age beats insertion order: with the cache full, the expired entry goes and no live one does.
			NetReconnectTxCache aging(2, 1000);
			NetH4TxKey slotA = key;
			slotA.stableSeat = 0;
			NetH4TxKey slotB = key;
			slotB.stableSeat = 1;
			aging.Store(Ramp<16>(1), slotA, NetH4LeaveAck{c_NetH4Version, Ramp<16>(1), 0, 7, true}, 0);
			aging.Store(Ramp<16>(2), slotB, NetH4LeaveAck{c_NetH4Version, Ramp<16>(2), 1, 7, true}, 900);
			if (aging.Size() != 2) {
				return Fail("the aging cache was not full before the contended insert");
			}
			NetH4TxKey slotC = key;
			slotC.stableSeat = 2;
			aging.Store(Ramp<16>(3), slotC, NetH4LeaveAck{c_NetH4Version, Ramp<16>(3), 2, 7, true}, 1600);
			if (aging.GetExpiredEvictions() != 1 || aging.GetCapacityEvictions() != 0) {
				return Fail("the expired entry was not preferred over the live one");
			}
			if (aging.Find(Ramp<16>(2), slotB, 1600) == nullptr || aging.Find(Ramp<16>(3), slotC, 1600) == nullptr) {
				return Fail("a live entry was evicted while an expired one remained");
			}
			return 0;
		}

		int TestUniformDenial() {
			ScriptedAuthCrypto provider;
			ScopedTestCrypto scoped(&provider);
			NetReconnectAdmission admission;
			const NetAuthBytes16 knownTx = Ramp<16>(0x10);
			const NetAuthBytes16 unknownTx = Ramp<16>(0x20);
			const NetAuthBytes16 staleTx = Ramp<16>(0x30);
			NetAuthBytes32 challenge{};

			// Three connections, three reasons, one clock: a known seat answering badly, an unknown
			// seat, and a Reclaim carrying a previous session's epoch.
			if (!admission.BeginAttempt(1, 5000) || !admission.BeginAttempt(2, 5000) || !admission.BeginAttempt(3, 5000)) {
				return Fail("a first attempt on a fresh connection was refused");
			}
			if (!admission.IssueChallenge(1, knownTx, 2, 7, 5000, challenge)) {
				return Fail("a known seat did not get a challenge");
			}
			if (!admission.IssueSyntheticChallenge(2, 5000, challenge)) {
				return Fail("an unknown seat did not get a synthetic challenge");
			}
			if (!admission.IssueSyntheticChallenge(3, 5000, challenge)) {
				return Fail("a stale epoch did not get a synthetic challenge");
			}
			// The synthetic flow retains nothing, so unknown-seat traffic cannot starve the pool.
			if (admission.GetOutstandingChallengeCount() != 1) {
				return Fail("a synthetic challenge drew a slot from the bounded pool");
			}

			NetH4ChallengeRecord consumed;
			if (!admission.ConsumeChallenge(1, knownTx, 5000, consumed)) {
				return Fail("the known seat's challenge could not be consumed");
			}
			admission.ScheduleDenial(1, knownTx, NetH4DenialReason::BadProof, 5000);
			admission.ScheduleDenial(2, unknownTx, NetH4DenialReason::UnknownSeat, 5000);
			admission.ScheduleDenial(3, staleTx, NetH4DenialReason::StaleEpoch, 5000);
			if (!admission.ReleaseDueDenials(5999).empty()) {
				return Fail("a denial was released before its scheduled time");
			}
			const std::vector<NetH4Denial> due = admission.ReleaseDueDenials(6000);
			if (due.size() != 3) {
				return Fail("the three denials did not release together: " + std::to_string(due.size()));
			}
			for (const NetH4Denial& denial : due) {
				if (denial.releaseAtMs != 6000 || denial.issuedAtMs != 5000) {
					return Fail("a denial did not follow the fixed issue+release schedule");
				}
			}
			if (admission.GetPendingDenialCount() != 0) {
				return Fail("a released denial stayed pending");
			}

			// One attempt per connection per second, and one pending denial per connection.
			if (admission.BeginAttempt(1, 5999)) {
				return Fail("a second attempt inside the connection's window was allowed");
			}
			if (!admission.BeginAttempt(1, 6000)) {
				return Fail("an attempt at the window boundary was refused");
			}
			admission.ScheduleDenial(1, knownTx, NetH4DenialReason::BadProof, 6000);
			admission.ScheduleDenial(1, unknownTx, NetH4DenialReason::UnknownSeat, 6000);
			if (admission.GetPendingDenialCount() != 1 || admission.GetCoalescedDenials() != 1) {
				return Fail("a connection accumulated more than one pending denial");
			}
			if (admission.GetRateLimitedAttempts() != 1) {
				return Fail("the refused attempt was not counted");
			}
			admission.ReleaseDueDenials(7000);

			// The hosted session's own budget, four attempts per interval.
			NetReconnectAdmission budget;
			for (NetPeerId connection = 1; connection <= 4; ++connection) {
				if (!budget.BeginAttempt(connection, 100)) {
					return Fail("the session budget refused a legitimate seat's attempt");
				}
			}
			if (budget.BeginAttempt(5, 100)) {
				return Fail("the session budget was exceeded");
			}
			if (!budget.BeginAttempt(5, 1100)) {
				return Fail("the session budget did not reopen after its interval");
			}
			return 0;
		}

		int TestChallengeLifecycle() {
			ScriptedAuthCrypto provider;
			ScopedTestCrypto scoped(&provider);
			NetReconnectAdmission admission;
			const NetAuthBytes16 txId = Ramp<16>(0x10);
			NetAuthBytes32 challenge{};
			NetH4ChallengeRecord consumed;

			if (!admission.IssueChallenge(1, txId, 2, 7, 0, challenge)) {
				return Fail("challenge issuance refused");
			}
			// Connection-bound: another socket holding the same id proves nothing.
			if (admission.ConsumeChallenge(2, txId, 0, consumed)) {
				return Fail("a challenge was consumed from another connection");
			}
			if (!admission.ConsumeChallenge(1, txId, NetReconnectAdmission::c_ChallengeLifetimeMs, consumed)) {
				return Fail("a challenge expired inside its lifetime");
			}
			if (consumed.stableSeat != 2 || consumed.holderGeneration != 7 || consumed.challenge != challenge) {
				return Fail("the consumed challenge did not carry its issued context");
			}
			// Consumed on the first attempt, so a replay is an expired challenge, not a second try.
			if (admission.ConsumeChallenge(1, txId, NetReconnectAdmission::c_ChallengeLifetimeMs, consumed)) {
				return Fail("a challenge was consumed twice");
			}

			if (!admission.IssueChallenge(1, txId, 2, 7, 0, challenge)) {
				return Fail("re-issuance refused");
			}
			if (admission.ConsumeChallenge(1, txId, NetReconnectAdmission::c_ChallengeLifetimeMs + 1, consumed)) {
				return Fail("a challenge outlived its pinned lifetime");
			}
			if (admission.GetExpiredChallenges() != 1) {
				return Fail("challenge expiry was not counted");
			}

			// Two live challenges per connection, eight across the session.
			NetReconnectAdmission bounded;
			for (uint8_t i = 0; i < 2; ++i) {
				if (!bounded.IssueChallenge(1, Ramp<16>(static_cast<uint8_t>(i + 1)), 2, 7, 0, challenge)) {
					return Fail("the per-connection allowance refused a retry overlap");
				}
			}
			if (bounded.IssueChallenge(1, Ramp<16>(3), 2, 7, 0, challenge)) {
				return Fail("a connection held more than two challenges");
			}
			for (NetPeerId connection = 2; connection <= 4; ++connection) {
				for (uint8_t i = 0; i < 2; ++i) {
					if (!bounded.IssueChallenge(connection, Ramp<16>(static_cast<uint8_t>(connection * 16 + i)), 2, 7, 0, challenge)) {
						return Fail("a seat could not reclaim while others were reclaiming");
					}
				}
			}
			if (bounded.GetOutstandingChallengeCount() != NetReconnectAdmission::c_MaxChallengesGlobal) {
				return Fail("the global pool did not fill to its bound");
			}
			if (bounded.IssueChallenge(5, Ramp<16>(0x90), 2, 7, 0, challenge)) {
				return Fail("the global challenge bound was exceeded");
			}
			// A full pool still answers an unknown seat, because that path stores nothing.
			if (!bounded.IssueSyntheticChallenge(5, 0, challenge)) {
				return Fail("a synthetic challenge was refused by the stateful bound");
			}
			bounded.DropConnection(1);
			if (bounded.GetOutstandingChallengeCount() != NetReconnectAdmission::c_MaxChallengesGlobal - 2) {
				return Fail("dropping a connection did not free its challenges");
			}
			return 0;
		}

		// Exercises every surface that touches auth material with the console captured, then proves no
		// secret reached it in raw or hex form. The positive control keeps the scan honest.
		int TestSecretCanary() {
			ScriptedAuthCrypto provider;
			ScopedTestCrypto scoped(&provider);
			NetSeatAuthRegistry registry;
			if (!registry.BeginHostedSession()) {
				return Fail("registry refused to arm for the canary run");
			}
			uint32_t generation = 0;
			NetSeatCredential credential{};
			if (!registry.IssueCredential(2, generation, credential)) {
				return Fail("credential issuance refused for the canary run");
			}
			const NetAuthEpoch epoch = registry.GetEpoch();

			NetAuthBytes32 challenge{};
			NetAuthBytes32 mac{};
			NetAuthBytes16 nonce{};
			NetAuthBytes16 txId{};
			std::string captured;
			bool flowRan = false;
			{
				ScopedConsoleCapture capture;
				NetReconnectAdmission admission;
				NetH4ChallengeRecord consumed;
				NetReconnectTxCache cache;
				if (NetH4DrawTxId(txId) && NetH4DrawNonce(nonce) && admission.BeginAttempt(1, 0) &&
				    admission.IssueChallenge(1, txId, 2, generation, 0, challenge) &&
				    admission.ConsumeChallenge(1, txId, 0, consumed)) {
					NetH4Transcript transcript;
					transcript.epoch = epoch;
					transcript.stableSeat = 2;
					transcript.holderGeneration = generation;
					transcript.challenge = consumed.challenge;
					transcript.clientNonce = nonce;
					(void)NetH4ComputeProof(credential, transcript, mac);
					(void)NetH4VerifyProof(credential, transcript, mac);
					(void)registry.MatchesActiveCredential(2, generation, credential);
					NetH4TxKey key;
					key.stableSeat = 2;
					key.holderGeneration = generation;
					key.identity = MakeIdentity();
					cache.Store(txId, key, NetH4JoinCommitted{c_NetH4Version, txId, 2, generation, 1, 2}, 0);
					(void)cache.Find(txId, key, 0);
					admission.ScheduleDenial(1, txId, NetH4DenialReason::BadProof, 0);
					(void)admission.ReleaseDueDenials(1000);
					std::vector<uint8_t> encoded;
					(void)NetProtocol::Encode({1, 0, NetH4TicketOffer{c_NetH4Version, txId, epoch, 2, generation, credential, 7, 20000}}, encoded);
					(void)NetProtocol::Decode(encoded);
					(void)NetProtocol::Encode({2, 0, NetH4Proof{c_NetH4Version, txId, epoch, 2, generation, nonce, mac}}, encoded);
					(void)NetProtocol::Decode(encoded);
					flowRan = true;
				}
				captured = capture.Text();
			}
			// Without this the scan would pass on an empty capture because nothing ran.
			if (!flowRan) {
				return Fail("the canary run did not reach the surfaces it scans");
			}

			struct Secret {
				const char* name;
				const uint8_t* bytes;
				size_t count;
			};
			const std::vector<Secret> secrets = {
				{"credential", credential.data(), credential.size()},
				{"challenge", challenge.data(), challenge.size()},
				{"proof mac", mac.data(), mac.size()},
				{"epoch", epoch.data(), epoch.size()},
				{"client nonce", nonce.data(), nonce.size()},
			};
			const auto scan = [](const std::string& text, const Secret& secret) {
				const std::string raw(reinterpret_cast<const char*>(secret.bytes), secret.count);
				return text.find(raw) != std::string::npos ||
				       text.find(HexOf(secret.bytes, secret.count, false)) != std::string::npos ||
				       text.find(HexOf(secret.bytes, secret.count, true)) != std::string::npos;
			};
			for (const Secret& secret : secrets) {
				if (scan(captured, secret)) {
					return Fail(std::string("the ") + secret.name + " reached the console");
				}
			}

			// Positive control: the scan must find a secret that IS printed, in each form it looks for.
			for (const Secret& secret : secrets) {
				const std::string rawLeak(reinterpret_cast<const char*>(secret.bytes), secret.count);
				if (!scan("prefix" + rawLeak + "suffix", secret) ||
				    !scan("prefix" + HexOf(secret.bytes, secret.count, false) + "suffix", secret) ||
				    !scan("prefix" + HexOf(secret.bytes, secret.count, true) + "suffix", secret)) {
					return Fail(std::string("the canary scan missed a planted ") + secret.name);
				}
			}
			return 0;
		}

		// Phase B's credential lifecycle: a reservation moves nothing, an adoption installs exactly the
		// credential the substitute was handed, and the superseded one can only ever refuse.
		int TestSubstitutionCredentialLifecycle() {
			ScriptedAuthCrypto provider;
			ScopedTestCrypto scoped(&provider);
			NetSeatAuthRegistry registry;
			if (registry.PeekNextGeneration(2) != 0) {
				return Fail("an inactive registry offered a generation");
			}
			if (!registry.BeginHostedSession()) {
				return Fail("the registry could not arm a hosted session");
			}
			uint32_t holderGeneration = 0;
			NetSeatCredential original{};
			if (!registry.IssueCredential(2, holderGeneration, original) || holderGeneration != 1) {
				return Fail("the seat's first credential was not generation 1");
			}
			// Reserving is a question, not a change: the seat still answers to its current holder.
			if (registry.PeekNextGeneration(2) != 2 || registry.GetActiveGeneration(2) != 1) {
				return Fail("peeking at the next generation moved the seat");
			}
			if (registry.PeekNextGeneration(2) != 2) {
				return Fail("peeking twice moved the generation");
			}
			// Only the next generation may be adopted, so a reassignment can never rewind one.
			const NetSeatCredential substitute = Ramp<32>(0x70);
			if (registry.AdoptCredential(2, 1, substitute) || registry.AdoptCredential(2, 3, substitute) ||
			    registry.AdoptCredential(2, 0, substitute)) {
				return Fail("a credential was adopted at a generation other than the next one");
			}
			if (registry.GetActiveGeneration(2) != 1 || !registry.MatchesActiveCredential(2, 1, original)) {
				return Fail("a refused adoption disturbed the seat");
			}
			registry.RetireCredentialForSubstitution(2);
			if (!registry.AdoptCredential(2, 2, substitute)) {
				return Fail("the substitute's own credential could not be installed at the next generation");
			}
			if (registry.GetActiveGeneration(2) != 2 || !registry.MatchesActiveCredential(2, 2, substitute)) {
				return Fail("the adopted credential is not the one the substitute holds");
			}
			if (registry.MatchesActiveCredential(2, 1, original)) {
				return Fail("the superseded credential still claims the seat");
			}

			// The retired credential answers - and nothing else. It cannot claim; it can only be told
			// apart from a stranger, which is the whole reason it is kept.
			NetH4Transcript transcript = MakeTranscript();
			transcript.stableSeat = 2;
			transcript.holderGeneration = 1;
			NetAuthBytes32 mac{};
			if (!NetH4ComputeProof(original, transcript, mac)) {
				return Fail("the superseded holder could not compute its own proof");
			}
			if (!registry.HasRetiredGeneration(2, 1) || registry.HasRetiredGeneration(2, 2) || registry.HasRetiredGeneration(3, 1)) {
				return Fail("the retired generation is not the one that was superseded");
			}
			if (!registry.VerifyRetiredProof(2, 1, transcript, mac)) {
				return Fail("the superseded holder's proof did not verify against the retired credential");
			}
			if (registry.VerifySeatProof(2, 1, transcript, mac)) {
				return Fail("a retired credential verified as a live one");
			}
			NetAuthBytes32 tampered = mac;
			tampered[0] = static_cast<uint8_t>(tampered[0] ^ 0x01U);
			if (registry.VerifyRetiredProof(2, 1, transcript, tampered) || registry.VerifyRetiredProof(2, 2, transcript, mac)) {
				return Fail("the retired credential verified something it should not have");
			}
			registry.ClearRetired(2);
			if (registry.HasRetiredGeneration(2, 1) || registry.VerifyRetiredProof(2, 1, transcript, mac)) {
				return Fail("the retired credential outlived ClearRetired");
			}
			if (registry.GetActiveGeneration(2) != 2) {
				return Fail("clearing the retired credential disturbed the live one");
			}

			// Revoking and ending the session take the retired credential with them.
			registry.RetireCredentialForSubstitution(2);
			registry.RevokeSeat(2);
			if (registry.HasRetiredGeneration(2, 2)) {
				return Fail("a revoked seat kept its retired credential");
			}
			uint32_t third = 0;
			NetSeatCredential fresh{};
			if (!registry.IssueCredential(2, third, fresh) || third != 3) {
				return Fail("generations rewound after a substitution");
			}
			registry.RetireCredentialForSubstitution(2);
			registry.EndSession();
			if (registry.HasRetiredGeneration(2, 3) || registry.PeekNextGeneration(2) != 0) {
				return Fail("the hosted session's end left credentials behind");
			}
			return 0;
		}

		// The precise refusal Phase B owes a loser rides the SAME schedule as every uniform denial, so
		// the timing an observer can measure is unchanged.
		int TestPreciseDenialKeepsUniformTiming() {
			ScriptedAuthCrypto provider;
			ScopedTestCrypto scoped(&provider);
			NetReconnectAdmission admission;
			const NetAuthBytes16 preciseTx = Ramp<16>(0x10);
			const NetAuthBytes16 uniformTx = Ramp<16>(0x20);
			const NetPayload refusal = NetJoinRejected{NetRejectReason::SeatReassigned, "this seat was given to another player", "seat_reassigned", "", ""};

			admission.ScheduleDenial(1, preciseTx, NetH4DenialReason::SeatReassigned, 5000, &refusal);
			admission.ScheduleDenial(2, uniformTx, NetH4DenialReason::UnknownSeat, 5000);
			if (!admission.ReleaseDueDenials(5999).empty()) {
				return Fail("a precise refusal was released early");
			}
			const std::vector<NetH4Denial> due = admission.ReleaseDueDenials(6000);
			if (due.size() != 2) {
				return Fail("the precise and uniform refusals did not release together");
			}
			for (const NetH4Denial& denial : due) {
				if (denial.issuedAtMs != 5000 || denial.releaseAtMs != 6000) {
					return Fail("a refusal moved off the fixed issue+release schedule");
				}
				const bool shouldBePrecise = denial.connection == 1;
				if (denial.precise != shouldBePrecise) {
					return Fail("a refusal carried the wrong wording");
				}
				if (shouldBePrecise && !(denial.payload == refusal)) {
					return Fail("the precise refusal was not the one that was scheduled");
				}
			}

			// A precise refusal upgrades a uniform one already pending on the connection - and keeps
			// the release time the first one set, so the upgrade is invisible to a clock.
			admission.ScheduleDenial(3, uniformTx, NetH4DenialReason::BadProof, 7000);
			admission.ScheduleDenial(3, preciseTx, NetH4DenialReason::SeatReassigned, 7400, &refusal);
			const std::vector<NetH4Denial> upgraded = admission.ReleaseDueDenials(8000);
			if (upgraded.size() != 1 || !upgraded.front().precise || upgraded.front().releaseAtMs != 8000) {
				return Fail("a coalesced precise refusal lost either its wording or its schedule");
			}
			if (admission.GetCoalescedDenials() != 1) {
				return Fail("the coalesced refusal was not counted");
			}
			return 0;
		}
	} // namespace

	int NetReconnectSelfTest::Run() {
		if (const int result = TestTranscriptLayout(); result != 0) {
			return result;
		}
#ifdef CCCP_WITH_GNS
		if (!GetNetAuthCrypto().IsRealCrypto()) {
			return Fail("GNS build default provider must be real crypto");
		}
		if (const int result = TestKnownAnswers(); result != 0) {
			return result;
		}
#else
		// No crypto library, so nothing may be issued and no proof may pass.
		if (GetNetAuthCrypto().IsRealCrypto()) {
			return Fail("GNS-less default provider must fail closed");
		}
		{
			NetAuthBytes32 mac{};
			if (NetH4ComputeProof(Ramp<32>(0), MakeTranscript(), mac) || NetH4VerifyProof(Ramp<32>(0), MakeTranscript(), mac)) {
				return Fail("a proof was produced without a crypto provider");
			}
			NetAuthBytes32 challenge{};
			if (NetH4DrawChallenge(challenge)) {
				return Fail("a challenge was drawn without a crypto provider");
			}
		}
#endif
		if (const int result = TestFailureInjection(); result != 0) {
			return result;
		}
		if (const int result = TestTxCache(); result != 0) {
			return result;
		}
		if (const int result = TestUniformDenial(); result != 0) {
			return result;
		}
		if (const int result = TestChallengeLifecycle(); result != 0) {
			return result;
		}
		if (const int result = TestSecretCanary(); result != 0) {
			return result;
		}
		if (const int result = TestSubstitutionCredentialLifecycle(); result != 0) {
			return result;
		}
		if (const int result = TestPreciseDenialKeepsUniformTiming(); result != 0) {
			return result;
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

		std::cout << "[net-reconnect-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
