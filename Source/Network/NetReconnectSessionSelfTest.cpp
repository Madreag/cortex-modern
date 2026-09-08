#include "NetReconnectSessionSelfTest.h"

#include "ControllerFrame.h"
#include "LoopbackTransport.h"
#include "NetAuthCrypto.h"
#include "NetLobbySession.h"
#include "NetLockstep.h"
#include "NetMatchConfig.h"
#include "NetReconnectLedger.h"
#include "NetReconnectSession.h"
#include "NetReconnectTicketStore.h"
#include "NetReconnectTranscript.h"
#include "NetSeatAuth.h"
#include "NetSession.h"
#include "System/System.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

	namespace {
		int Fail(const std::string& message) {
			std::cerr << "[net-reconnect-session-selftest] FAIL: " << message << std::endl;
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

		// Deterministic test-only provider, installed only through SetNetAuthCryptoForTest. The mac is a
		// plain fold, NOT cryptographic; the real HMAC has its known-answer vectors in -net-reconnect-selftest.
		class ScriptedAuthCrypto : public NetAuthCrypto {
		public:
			bool failRandom = false;
			bool failHmac = false;

			bool IsRealCrypto() const override { return false; }

			// Deliberately not a ramp: the canary scans look for auth material inside artifacts that are
			// full of ramp-shaped test hashes, and a ramp secret would be a substring of one.
			bool RandomBytes(uint8_t* buffer, size_t count) override {
				if (failRandom || buffer == nullptr) {
					return false;
				}
				for (size_t i = 0; i < count; ++i) {
					m_Counter = static_cast<uint8_t>(m_Counter * 37U + 149U);
					buffer[i] = m_Counter;
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

		NetHash32 MakeHash(uint8_t seed) {
			return Ramp<32>(seed);
		}

		NetH4Identity MakeIdentity() {
			NetH4Identity identity;
			identity.controllerFrameVersion = ControllerFrame::c_Version;
			identity.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			identity.gameVersion = "7.0.0-test";
			identity.buildId = "stage2-h4a2";
			identity.deterministicConfigHash = MakeHash(1);
			identity.moduleManifestHash = MakeHash(33);
			identity.sessionRulesHash = MakeHash(65);
			identity.sessionIdentityHash = MakeHash(97);
			return identity;
		}

		std::vector<NetH4Seat> MakeSeatTable() {
			// Two human seats plus one CPU slot, keyed by slot index the way a live match config is. The
			// session id a commit hands back and the lockstep id the ledger names are deliberately
			// different numbers here, because in a real match config they are.
			return {{0, 1, 1, false, 2, false}, {1, 2, 2, false, 3, false}, {2, 0, 3, true, 0, false}};
		}

		// Every test writes its store beneath the private runtime's Userdata, never the repo's. The
		// selftests run before System::Initialize resolves its paths, so the runtime's own cwd is it.
		std::filesystem::path LaneDirectory() {
			return std::filesystem::current_path() / "Userdata" / "h4-a2-selftest";
		}

		std::string StorePath(const std::string& name) {
			return (LaneDirectory() / (name + ".ticket")).string();
		}

		bool ResetLaneDirectory(std::string* error) {
			std::error_code code;
			std::filesystem::remove_all(LaneDirectory(), code);
			std::filesystem::create_directories(LaneDirectory(), code);
			if (code) {
				*error = "could not prepare the selftest ticket directory: " + code.message();
				return false;
			}
			return true;
		}

		// One connected client: its own ticket store and its own transport id on the host.
		struct Endpoint {
			NetPeerId connection = c_InvalidNetPeerId;
			NetReconnectTicketStore store;
			NetReconnectClient client;
			bool connected = true;
		};

		// Moves every payload through the real encoder and decoder, so the transaction under test rides
		// exactly the bytes the wire carries.
		class Wire {
		public:
			NetSeatAuthRegistry registry;
			NetReconnectHost host;
			uint64_t nowMs = 0;
			uint32_t droppedToFence = 0;

			void Add(Endpoint* endpoint) { m_Endpoints.push_back(endpoint); }
			void Remove(NetPeerId connection) {
				m_Endpoints.erase(std::remove_if(m_Endpoints.begin(), m_Endpoints.end(), [connection](Endpoint* endpoint) {
					return endpoint->connection == connection;
				}), m_Endpoints.end());
			}

			const std::vector<NetPayload>& Delivered(NetPeerId connection) { return m_Delivered[connection]; }
			void ClearDelivered() { m_Delivered.clear(); }

			bool Pump(std::string* error, uint32_t maxRounds = 16) {
				for (uint32_t round = 0; round < maxRounds; ++round) {
					host.Tick(nowMs);
					for (Endpoint* endpoint : m_Endpoints) {
						endpoint->client.Tick(nowMs);
					}
					bool moved = false;
					for (NetH4Outbound& outbound : host.TakeOutbound()) {
						NetPayload payload;
						if (!Roundtrip(outbound.payload, payload, error)) {
							return false;
						}
						m_Delivered[outbound.connection].push_back(payload);
						moved = true;
						// The session drops a fenced transport's traffic before it reaches anyone.
						Endpoint* endpoint = Find(outbound.connection);
						if (endpoint != nullptr && endpoint->connected) {
							endpoint->client.HandleMessage(payload, nowMs);
						}
					}
					host.TakeCommits();
					for (Endpoint* endpoint : m_Endpoints) {
						for (NetH4Outbound& outbound : endpoint->client.TakeOutbound()) {
							NetPayload payload;
							if (!Roundtrip(outbound.payload, payload, error)) {
								return false;
							}
							moved = true;
							if (!endpoint->connected) {
								continue;
							}
							if (host.IsFenced(endpoint->connection)) {
								++droppedToFence;
								host.CountFencedPacket();
								continue;
							}
							host.HandleMessage(endpoint->connection, payload, nowMs);
						}
					}
					if (!moved) {
						return true;
					}
				}
				*error = "the admission wire never settled";
				return false;
			}

			/// Carries one message each way and hands back what the client would have replied, so a test
			/// can lose exactly that reply.
			bool Step(Endpoint& endpoint, std::vector<NetPayload>& clientReplies, std::string* error) {
				clientReplies.clear();
				for (NetH4Outbound& outbound : endpoint.client.TakeOutbound()) {
					NetPayload payload;
					if (!Roundtrip(outbound.payload, payload, error)) {
						return false;
					}
					host.HandleMessage(endpoint.connection, payload, nowMs);
				}
				host.Tick(nowMs);
				for (NetH4Outbound& outbound : host.TakeOutbound()) {
					NetPayload payload;
					if (!Roundtrip(outbound.payload, payload, error)) {
						return false;
					}
					m_Delivered[outbound.connection].push_back(payload);
					if (outbound.connection == endpoint.connection) {
						endpoint.client.HandleMessage(payload, nowMs);
					}
				}
				host.TakeCommits();
				for (NetH4Outbound& outbound : endpoint.client.TakeOutbound()) {
					NetPayload payload;
					if (!Roundtrip(outbound.payload, payload, error)) {
						return false;
					}
					clientReplies.push_back(payload);
				}
				return true;
			}

			/// Delivers whatever the host owes right now, without letting the clients answer. The
			/// timing tests need the host's replies at an exact tick, not a settled conversation.
			void DrainHostOutbound() {
				host.Tick(nowMs);
				for (NetH4Outbound& outbound : host.TakeOutbound()) {
					NetPayload payload;
					std::string ignored;
					if (Roundtrip(outbound.payload, payload, &ignored)) {
						m_Delivered[outbound.connection].push_back(payload);
					}
				}
				host.TakeCommits();
			}

			bool SendRaw(NetPeerId connection, const NetPayload& payload, std::string* error) {
				NetPayload decoded;
				if (!Roundtrip(payload, decoded, error)) {
					return false;
				}
				host.HandleMessage(connection, decoded, nowMs);
				return true;
			}

			static bool Roundtrip(const NetPayload& payload, NetPayload& out, std::string* error) {
				std::vector<uint8_t> bytes;
				NetProtocolError encodeError;
				NetMessage message;
				message.sequence = 1;
				message.payload = payload;
				if (!NetProtocol::Encode(message, bytes, &encodeError)) {
					*error = "admission message did not encode: " + encodeError.message;
					return false;
				}
				const NetDecodeResult decoded = NetProtocol::Decode(bytes);
				if (!decoded.ok) {
					*error = "admission message did not decode: " + decoded.error.message;
					return false;
				}
				out = decoded.message.payload;
				return true;
			}

		private:
			Endpoint* Find(NetPeerId connection) {
				const auto found = std::find_if(m_Endpoints.begin(), m_Endpoints.end(), [connection](Endpoint* endpoint) {
					return endpoint->connection == connection;
				});
				return found == m_Endpoints.end() ? nullptr : *found;
			}

			std::vector<Endpoint*> m_Endpoints;
			std::map<NetPeerId, std::vector<NetPayload>> m_Delivered;
		};

		void ConfigureWire(Wire& wire, uint64_t hostSessionId = 0x4831ULL) {
			wire.registry.BeginHostedSession();
			wire.host.Configure(&wire.registry, hostSessionId, MakeIdentity());
			wire.host.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			wire.host.SetLiveMatch(false);
		}

		uint64_t FixedUnixClock(void* context) {
			return *static_cast<uint64_t*>(context);
		}

		void ConfigureEndpoint(Endpoint& endpoint, const std::string& name, uint64_t* unixClock) {
			endpoint.store.SetPath(StorePath(name));
			endpoint.client.Configure(&endpoint.store, MakeIdentity(), name);
			endpoint.client.SetUnixClock(&FixedUnixClock, unixClock);
		}

		template <typename T>
		size_t CountOf(const std::vector<NetPayload>& payloads) {
			return static_cast<size_t>(std::count_if(payloads.begin(), payloads.end(), [](const NetPayload& payload) {
				return std::holds_alternative<T>(payload);
			}));
		}

		template <typename T>
		const T* LastOf(const std::vector<NetPayload>& payloads) {
			for (auto it = payloads.rbegin(); it != payloads.rend(); ++it) {
				if (const T* found = std::get_if<T>(&*it)) {
					return found;
				}
			}
			return nullptr;
		}

		int TestTicketStore() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			NetReconnectTicketStore store;
			store.SetPath(StorePath("store"));

			NetH4TicketRecord record;
			record.recordVersion = NetReconnectTicketStore::c_RecordVersion;
			record.epoch = Ramp<16>(0x10);
			record.stableSeat = 1;
			record.holderGeneration = 3;
			record.credential = Ramp<32>(0x40);
			record.hostSessionId = 0x1122334455667788ULL;
			record.hostAddress = "203.0.113.7:41010";
			record.issuedAtUnixMs = 1'000'000'000'000ULL;
			record.matchConfigHash = MakeHash(7);

			if (!store.Store(record, &error)) {
				return Fail("the ticket store refused a well-formed record: " + error);
			}
			if (!store.HasRecord()) {
				return Fail("the ticket store wrote no record");
			}
			NetH4TicketRecord loaded;
			if (store.Load(record.issuedAtUnixMs + 1000, loaded, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail("the stored record did not load back: " + error);
			}
			if (!(loaded == record)) {
				return Fail("the loaded record differs from the stored one");
			}

			// One record only: a ticket for another session replaces it atomically.
			NetH4TicketRecord second = record;
			second.stableSeat = 0;
			second.holderGeneration = 9;
			second.hostSessionId = 0x99ULL;
			if (!store.Store(second, &error)) {
				return Fail("the ticket store refused the replacement: " + error);
			}
			if (store.Load(record.issuedAtUnixMs + 1000, loaded, &error) != NetH4TicketLoadResult::Loaded || !(loaded == second)) {
				return Fail("the replacement did not become the one record");
			}
			// The replace leaves no temporary behind for the canary scan to trip over.
			if (std::filesystem::exists(store.GetPath() + ".tmp")) {
				return Fail("the ticket store left its temporary file behind");
			}

			// A failed write must leave the previous valid record intact.
			NetH4TicketRecord malformed = second;
			malformed.holderGeneration = 0;
			if (store.Store(malformed, &error)) {
				return Fail("the ticket store accepted a record with no holder");
			}
			if (store.Load(record.issuedAtUnixMs + 1000, loaded, &error) != NetH4TicketLoadResult::Loaded || !(loaded == second)) {
				return Fail("a failed write destroyed the previous record");
			}

			// Past the outer bound the record is refused rather than offered for a dead match.
			if (store.Load(second.issuedAtUnixMs + NetReconnectTicketStore::c_MaxRecordAgeMs, loaded, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail("a record exactly at the age bound was refused");
			}
			if (store.Load(second.issuedAtUnixMs + NetReconnectTicketStore::c_MaxRecordAgeMs + 1, loaded, &error) != NetH4TicketLoadResult::Stale) {
				return Fail("a record past the age bound was still offered");
			}

			// A flipped byte fails the integrity mac, so a damaged record is refused, never trusted.
			{
				std::vector<uint8_t> bytes;
				{
					std::ifstream file(store.GetPath(), std::ios::binary);
					bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
				}
				if (bytes.size() < 40) {
					return Fail("the stored record is implausibly short");
				}
				std::vector<uint8_t> damaged = bytes;
				damaged[30] ^= 0x01U;
				{
					std::ofstream file(store.GetPath(), std::ios::binary);
					file.write(reinterpret_cast<const char*>(damaged.data()), static_cast<long>(damaged.size()));
				}
				if (store.Load(second.issuedAtUnixMs, loaded, &error) != NetH4TicketLoadResult::Corrupt) {
					return Fail("a damaged record loaded anyway");
				}
				// A flipped mac byte alone is enough.
				damaged = bytes;
				damaged.back() ^= 0x80U;
				{
					std::ofstream file(store.GetPath(), std::ios::binary);
					file.write(reinterpret_cast<const char*>(damaged.data()), static_cast<long>(damaged.size()));
				}
				if (store.Load(second.issuedAtUnixMs, loaded, &error) != NetH4TicketLoadResult::Corrupt) {
					return Fail("a record with a forged mac loaded anyway");
				}
				{
					std::ofstream file(store.GetPath(), std::ios::binary);
					file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<long>(bytes.size()));
				}
			}

			if (!store.Clear(&error) || store.HasRecord()) {
				return Fail("the record survived a clear");
			}
			if (store.Load(second.issuedAtUnixMs, loaded, &error) != NetH4TicketLoadResult::Missing) {
				return Fail("a cleared store still reported a record");
			}

			// A ticket record can never verify as a reclaim proof: the domain tags differ.
			{
				std::vector<uint8_t> body;
				if (!NetReconnectTicketStore::Serialize(second, body)) {
					return Fail("the record did not serialize for the domain check");
				}
				NetAuthBytes32 ticketMac{};
				if (!NetH4MacTicketRecord(second.credential, body, ticketMac)) {
					return Fail("the record mac could not be computed");
				}
				NetH4Transcript transcript;
				transcript.domain = NetH4ProofDomain::Reclaim;
				transcript.epoch = second.epoch;
				transcript.stableSeat = second.stableSeat;
				transcript.holderGeneration = second.holderGeneration;
				transcript.challenge = Ramp<32>(0x20);
				transcript.clientNonce = Ramp<16>(0x50);
				if (NetH4VerifyProof(second.credential, transcript, ticketMac)) {
					return Fail("a ticket record mac verified as a reclaim proof");
				}
			}
			return 0;
		}

		// P19: the canary carve-out is exactly one resolved path, and its presence is asserted whenever a
		// ticket was issued - otherwise the exclusion could silently disarm the whole scan.
		int TestTicketArtifactCanary() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			Wire wire;
			ConfigureWire(wire);
			uint64_t unixNow = 1'700'000'000'000ULL;
			Endpoint player;
			player.connection = 41;
			ConfigureEndpoint(player, "canary", &unixNow);
			wire.Add(&player);

			if (!player.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the canary join did not complete: " + error);
			}
			if (player.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the canary join did not commit");
			}

			const std::string allowed = player.store.GetPath();
			// Required presence: a ticket was issued, so the allowlisted path must actually exist.
			if (!std::filesystem::exists(allowed)) {
				return Fail("a ticket was issued but the allowlisted store path does not exist");
			}

			NetH4TicketRecord record;
			if (player.store.Load(unixNow, record, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail("the issued ticket did not load for the canary scan: " + error);
			}
			const std::vector<std::pair<const char*, std::vector<uint8_t>>> secrets = {
				{"credential", {record.credential.begin(), record.credential.end()}},
				{"epoch", {record.epoch.begin(), record.epoch.end()}},
			};
			const auto scan = [&secrets](const std::vector<uint8_t>& bytes, std::string& hit) {
				const std::string text(bytes.begin(), bytes.end());
				for (const auto& [name, secret] : secrets) {
					const std::string raw(secret.begin(), secret.end());
					if (text.find(raw) != std::string::npos ||
					    text.find(HexOf(secret.data(), secret.size(), false)) != std::string::npos ||
					    text.find(HexOf(secret.data(), secret.size(), true)) != std::string::npos) {
						hit = name;
						return true;
					}
				}
				return false;
			};

			// Every artifact in the lane directory except the one allowlisted path must be clean.
			size_t scanned = 0;
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(LaneDirectory())) {
				if (!entry.is_regular_file()) {
					continue;
				}
				if (std::filesystem::equivalent(entry.path(), std::filesystem::path(allowed))) {
					continue;
				}
				std::ifstream file(entry.path(), std::ios::binary);
				const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				std::string hit;
				++scanned;
				if (scan(bytes, hit)) {
					return Fail("the " + hit + " reached artifact " + entry.path().string());
				}
			}

			// Positive control: the scanner really does find the secrets, in the file it is allowed to skip.
			{
				std::ifstream file(allowed, std::ios::binary);
				const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				std::string hit;
				if (!scan(bytes, hit)) {
					return Fail("the artifact scan cannot see a secret even in the ticket file itself");
				}
			}
			(void)scanned;
			return 0;
		}

		int TestFirstJoinTransaction() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			Wire wire;
			ConfigureWire(wire);
			uint64_t unixNow = 1'700'000'000'000ULL;
			Endpoint player;
			player.connection = 11;
			ConfigureEndpoint(player, "join", &unixNow);
			wire.Add(&player);

			if (!player.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the first join did not settle: " + error);
			}
			if (player.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the first join did not reach a commit");
			}
			if (player.client.GetIncarnation() != 1) {
				return Fail("the first commit was not incarnation 1");
			}
			// P4: the seat's own recorded peer id, never a freshly allocated one.
			if (player.client.GetAssignedPeerId() != MakeSeatTable()[0].peerId) {
				return Fail("the commit did not use the seat's recorded peer id");
			}
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(0, holder, generation, incarnation) || holder != player.connection || generation != 1 || incarnation != 1) {
				return Fail("the seat did not record its holder");
			}
			NetH4TicketRecord record;
			if (player.store.Load(unixNow, record, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail("the committed join left no durable ticket: " + error);
			}
			if (record.stableSeat != 0 || record.holderGeneration != 1 || record.hostSessionId != 0x4831ULL ||
			    !(record.epoch == wire.registry.GetEpoch())) {
				return Fail("the stored ticket does not describe the committed seat");
			}
			if (wire.host.GetProvisionalSeatCount() != 0) {
				return Fail("the provisional seat outlived its commit");
			}

			// A duplicate ack replays the same terminal result rather than opening a second transaction.
			const std::vector<NetPayload>& delivered = wire.Delivered(player.connection);
			const NetH4JoinCommitted* committed = LastOf<NetH4JoinCommitted>(delivered);
			if (committed == nullptr) {
				return Fail("no JoinCommitted was delivered");
			}
			const NetH4JoinCommitted first = *committed;
			const uint32_t replaysBefore = wire.host.GetStats().replayedResults;
			if (!wire.SendRaw(player.connection, NetH4TicketStoredAck{c_NetH4Version, first.txId, first.stableSeat, first.holderGeneration, true}, &error)) {
				return Fail(error);
			}
			if (!wire.Pump(&error)) {
				return Fail(error);
			}
			const NetH4JoinCommitted* replayed = LastOf<NetH4JoinCommitted>(wire.Delivered(player.connection));
			if (replayed == nullptr || !(*replayed == first)) {
				return Fail("a duplicate ack did not replay the identical commit");
			}
			if (wire.host.GetStats().replayedResults != replaysBefore + 1) {
				return Fail("the replay was not counted");
			}
			if (wire.host.GetStats().provisionalSeatsCommitted != 1) {
				return Fail("the duplicate ack committed a second seat");
			}
			return 0;
		}

		int TestProvisionalFailureWindows() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;

			// (a) The offer is persisted but the ack dies with the link; the client resumes on a fresh one.
			{
				Wire wire;
				ConfigureWire(wire);
				Endpoint first;
				first.connection = 21;
				ConfigureEndpoint(first, "resume", &unixNow);
				wire.Add(&first);
				if (!first.client.BeginNewJoin(wire.nowMs, &error)) {
					return Fail(error);
				}
				// The offer lands and is persisted, but the ack it produces never reaches the host.
				std::vector<NetPayload> lostAck;
				if (!wire.Step(first, lostAck, &error)) {
					return Fail(error);
				}
				if (lostAck.size() != 1 || !std::holds_alternative<NetH4TicketStoredAck>(lostAck.front())) {
					return Fail("the offer did not produce exactly one ack to lose");
				}
				if (!first.store.HasRecord()) {
					return Fail("the offer was not persisted before the ack");
				}
				if (wire.host.NotifyDisconnect(first.connection, 0) != NetH4DisconnectOutcome::Unknown) {
					return Fail("a disconnect before the commit was read as a seat drop");
				}
				NetPeerId holder = c_InvalidNetPeerId;
				uint32_t generation = 0;
				uint32_t incarnation = 0;
				if (wire.host.GetSeatHolder(0, holder, generation, incarnation)) {
					return Fail("a disconnect before the commit left a committed seat");
				}
				// The client relaunches inside the resume window and re-presents the same ack.
				NetH4TicketRecord record;
				if (first.store.Load(unixNow, record, &error) != NetH4TicketLoadResult::Loaded) {
					return Fail("the persisted offer did not load: " + error);
				}
				Endpoint second;
				second.connection = 22;
				ConfigureEndpoint(second, "resume", &unixNow);
				wire.Remove(first.connection);
				wire.Add(&second);
				wire.nowMs += 5000;
				const NetH4TicketOffer* offer = LastOf<NetH4TicketOffer>(wire.Delivered(first.connection));
				if (offer == nullptr) {
					return Fail("no ticket offer was delivered to resume from");
				}
				if (!wire.SendRaw(second.connection, NetH4TicketStoredAck{c_NetH4Version, offer->txId, offer->stableSeat, offer->holderGeneration, true}, &error) ||
				    !wire.Pump(&error)) {
					return Fail(error);
				}
				if (!wire.host.GetSeatHolder(0, holder, generation, incarnation) || holder != second.connection) {
					return Fail("the resumed transaction did not commit the seat");
				}
				if (wire.host.GetStats().provisionalSeatsResumed != 1) {
					return Fail("the resume was not counted");
				}
			}

			// (b) Past the resume window the provisional expires and the orphan ticket is stale.
			{
				Wire wire;
				ConfigureWire(wire);
				Endpoint first;
				first.connection = 23;
				ConfigureEndpoint(first, "expire", &unixNow);
				wire.Add(&first);
				if (!first.client.BeginNewJoin(wire.nowMs, &error)) {
					return Fail(error);
				}
				std::vector<NetPayload> lostAck;
				if (!wire.Step(first, lostAck, &error)) {
					return Fail(error);
				}
				const NetH4TicketOffer* offered = LastOf<NetH4TicketOffer>(wire.Delivered(first.connection));
				if (offered == nullptr) {
					return Fail("no ticket offer to expire");
				}
				const NetH4TicketOffer offer = *offered;
				wire.host.NotifyDisconnect(first.connection, 0);
				wire.nowMs += NetReconnectHost::c_ProvisionalExpiryMs + 1;
				wire.host.Tick(wire.nowMs);
				wire.host.TakeOutbound();
				if (wire.host.GetStats().provisionalSeatsExpired != 1 || wire.host.GetProvisionalSeatCount() != 0) {
					return Fail("the provisional seat did not expire");
				}
				const uint32_t dropsBefore = wire.host.GetStats().unknownTransactionDrops;
				if (!wire.SendRaw(24, NetH4TicketStoredAck{c_NetH4Version, offer.txId, offer.stableSeat, offer.holderGeneration, true}, &error)) {
					return Fail(error);
				}
				if (wire.host.GetStats().unknownTransactionDrops != dropsBefore + 1) {
					return Fail("the orphan ack was not dropped as stale");
				}
				NetPeerId holder = c_InvalidNetPeerId;
				uint32_t generation = 0;
				uint32_t incarnation = 0;
				if (wire.host.GetSeatHolder(0, holder, generation, incarnation)) {
					return Fail("an expired provisional committed a seat anyway");
				}
				// The orphan ticket now takes the ordinary unknown-seat path.
				NetH4Reclaim reclaim;
				reclaim.txId = Ramp<16>(0x70);
				reclaim.epoch = offer.epoch;
				reclaim.stableSeat = offer.stableSeat;
				reclaim.holderGeneration = offer.holderGeneration;
				reclaim.identity = MakeIdentity();
				reclaim.displayName = "orphan";
				if (!wire.SendRaw(24, reclaim, &error)) {
					return Fail(error);
				}
				wire.nowMs += NetReconnectAdmission::c_DenialReleaseMs;
				if (!wire.Pump(&error)) {
					return Fail(error);
				}
				if (CountOf<NetH4Challenge>(wire.Delivered(24)) != 1 || CountOf<NetJoinRejected>(wire.Delivered(24)) != 1) {
					return Fail("the orphan reclaim did not take the synthetic-challenge denial path");
				}
			}

			// (c) A persistence failure blocks the join: the seat must not be committed to a client that
			// could never prove it again.
			{
				Wire wire;
				ConfigureWire(wire);
				Endpoint player;
				player.connection = 25;
				ConfigureEndpoint(player, "nostore", &unixNow);
				// A directory where the file should be: every write fails, the record never lands.
				std::error_code code;
				std::filesystem::create_directories(StorePath("nostore"), code);
				wire.Add(&player);
				if (!player.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
					return Fail("the blocked join did not settle: " + error);
				}
				if (player.client.GetState() != NetH4ClientState::Failed) {
					return Fail("a client that could not persist its ticket still joined");
				}
				NetPeerId holder = c_InvalidNetPeerId;
				uint32_t generation = 0;
				uint32_t incarnation = 0;
				if (wire.host.GetSeatHolder(0, holder, generation, incarnation)) {
					return Fail("a persistence failure still committed the seat");
				}
				if (wire.host.GetStats().persistenceFailures != 1) {
					return Fail("the persistence failure was not counted");
				}
				if (wire.registry.GetActiveGeneration(0) != 0) {
					return Fail("the blocked join left an active credential behind");
				}
				if (CountOf<NetJoinRejected>(wire.Delivered(player.connection)) != 1) {
					return Fail("the blocked join was not told why");
				}
			}
			return 0;
		}

		// Brings one client to a committed seat and then drops its link, which is the state every reclaim
		// test starts from.
		int SeatAndDrop(Wire& wire, Endpoint& player, NetH4TicketRecord& record, uint64_t unixNow, std::string* error) {
			if (!player.client.BeginNewJoin(wire.nowMs, error) || !wire.Pump(error)) {
				return 1;
			}
			if (player.client.GetState() != NetH4ClientState::Joined) {
				*error = "the seeding join did not commit";
				return 1;
			}
			if (player.store.Load(unixNow, record, error) != NetH4TicketLoadResult::Loaded) {
				return 1;
			}
			wire.host.NotifyDisconnect(player.connection, 100);
			player.connected = false;
			wire.Remove(player.connection);
			player.client.NotifyAmbiguousLoss();
			return 0;
		}

		int TestReclaimAndDuplicateProof() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint player;
			player.connection = 31;
			ConfigureEndpoint(player, "reclaim", &unixNow);
			wire.Add(&player);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, player, record, unixNow, &error) != 0) {
				return Fail("could not seat the player before the reclaim: " + error);
			}
			// The record is exactly what an ambiguous loss must not clear.
			if (!player.store.HasRecord()) {
				return Fail("an ambiguous loss cleared the recovery record");
			}
			if (wire.host.GetStats().seatsDropped != 1) {
				return Fail("the seat drop was not recorded");
			}

			wire.nowMs += 3000;
			wire.host.SetLiveMatch(true);
			Endpoint returner;
			returner.connection = 32;
			ConfigureEndpoint(returner, "reclaim", &unixNow);
			wire.Add(&returner);
			if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the reclaim did not settle: " + error);
			}
			if (returner.client.GetState() != NetH4ClientState::Joined) {
				return Fail("a valid ticket and proof did not reclaim the seat");
			}
			if (returner.client.GetAssignedPeerId() != MakeSeatTable()[0].peerId) {
				return Fail("the reclaim did not restore the seat's own peer id");
			}
			if (returner.client.GetIncarnation() != 2) {
				return Fail("the reclaim did not advance the incarnation");
			}
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(0, holder, generation, incarnation) || holder != returner.connection || generation != 1) {
				return Fail("the reclaim did not rebind the exact seat");
			}

			// A duplicate valid transaction (its JoinCommitted was lost) replays the success it earned.
			if (LastOf<NetH4Challenge>(wire.Delivered(returner.connection)) == nullptr) {
				return Fail("no challenge was delivered to the returner");
			}
			const NetH4JoinCommitted* committed = LastOf<NetH4JoinCommitted>(wire.Delivered(returner.connection));
			if (committed == nullptr) {
				return Fail("no commit was delivered to the returner");
			}
			const NetH4JoinCommitted firstCommit = *committed;
			const uint32_t replaysBefore = wire.host.GetStats().replayedResults;
			NetH4Reclaim duplicate;
			duplicate.txId = firstCommit.txId;
			duplicate.epoch = record.epoch;
			duplicate.stableSeat = record.stableSeat;
			duplicate.holderGeneration = record.holderGeneration;
			duplicate.identity = MakeIdentity();
			duplicate.displayName = "reclaim";
			if (!wire.SendRaw(returner.connection, duplicate, &error) || !wire.Pump(&error)) {
				return Fail(error);
			}
			if (wire.host.GetStats().replayedResults != replaysBefore + 1) {
				return Fail("a duplicate transaction did not replay its cached result");
			}
			const NetH4JoinCommitted* replay = LastOf<NetH4JoinCommitted>(wire.Delivered(returner.connection));
			if (replay == nullptr || !(*replay == firstCommit)) {
				return Fail("the replay was not byte-identical to the first commit");
			}
			if (wire.host.GetStats().reclaimsAccepted != 1) {
				return Fail("the duplicate ran a second reclaim");
			}
			// The incarnation did not move for a replay, so nothing was re-fenced.
			if (!wire.host.GetSeatHolder(0, holder, generation, incarnation) || incarnation != 2) {
				return Fail("a replayed result moved the incarnation");
			}
			return 0;
		}


		// §5's whole point: an unknown seat, a live seat under the wrong holder generation and a Reclaim
		// carrying a previous session's epoch must be indistinguishable from a known seat answering badly.
		int TestNoEnumerationOracle() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint player;
			player.connection = 41;
			ConfigureEndpoint(player, "oracle", &unixNow);
			wire.Add(&player);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, player, record, unixNow, &error) != 0) {
				return Fail("could not seat the player: " + error);
			}
			wire.host.SetLiveMatch(true);
			wire.ClearDelivered();

			const NetH4Identity identity = MakeIdentity();
			const auto reclaimFor = [&identity](const NetAuthBytes16& epoch, uint16_t seat, uint32_t generation, uint8_t txSeed) {
				NetH4Reclaim reclaim;
				reclaim.txId = Ramp<16>(txSeed);
				reclaim.epoch = epoch;
				reclaim.stableSeat = seat;
				reclaim.holderGeneration = generation;
				reclaim.identity = identity;
				reclaim.displayName = "claimant";
				return reclaim;
			};

			const uint64_t issuedAtMs = wire.nowMs;
			const std::vector<NetPeerId> connections = {51, 52, 53, 54};
			if (!wire.SendRaw(51, reclaimFor(record.epoch, 9, 1, 0x60), &error) ||   // a seat that does not exist
			    !wire.SendRaw(52, reclaimFor(record.epoch, 0, 9, 0x61), &error) ||   // the live seat, wrong generation
			    !wire.SendRaw(53, reclaimFor(Ramp<16>(0xA0), 0, 1, 0x62), &error) || // a previous session's epoch
			    !wire.SendRaw(54, reclaimFor(record.epoch, 0, 1, 0x63), &error)) {   // the live seat, about to prove badly
				return Fail(error);
			}
			wire.DrainHostOutbound();
			for (NetPeerId connection : connections) {
				if (CountOf<NetH4Challenge>(wire.Delivered(connection)) != 1) {
					return Fail("connection " + std::to_string(connection) + " did not get exactly one challenge");
				}
				const NetH4Challenge* challenge = LastOf<NetH4Challenge>(wire.Delivered(connection));
				if (challenge->challenge.size() != 32 || challenge->lifetimeMs != NetReconnectAdmission::c_ChallengeLifetimeMs) {
					return Fail("the challenge differed in shape by connection");
				}
				if (CountOf<NetJoinRejected>(wire.Delivered(connection)) != 0) {
					return Fail("a refusal arrived before its release time");
				}
			}
			// Only the known seat drew a stateful slot: unknown-seat spam cannot starve a real reclaimer.
			if (wire.host.GetAdmission().GetOutstandingChallengeCount() != 1) {
				return Fail("a synthetic challenge drew from the bounded pool");
			}
			if (wire.host.GetAdmission().GetSyntheticChallenges() != 3) {
				return Fail("the three refused flows did not each get a synthetic challenge");
			}

			wire.nowMs = issuedAtMs + NetReconnectAdmission::c_DenialReleaseMs - 1;
			wire.DrainHostOutbound();
			for (NetPeerId connection : {51, 52, 53}) {
				if (CountOf<NetJoinRejected>(wire.Delivered(connection)) != 0) {
					return Fail("a refusal was released early");
				}
			}
			wire.nowMs = issuedAtMs + NetReconnectAdmission::c_DenialReleaseMs;
			wire.DrainHostOutbound();
			const NetJoinRejected* reference = nullptr;
			for (NetPeerId connection : {51, 52, 53}) {
				if (CountOf<NetJoinRejected>(wire.Delivered(connection)) != 1) {
					return Fail("connection " + std::to_string(connection) + " was not refused on the shared schedule");
				}
				const NetJoinRejected* refusal = LastOf<NetJoinRejected>(wire.Delivered(connection));
				if (reference == nullptr) {
					reference = refusal;
				} else if (!(*refusal == *reference)) {
					return Fail("two refusals differ on the wire");
				}
			}

			// The known seat answers badly. Its refusal is the same message on the same delay from its own
			// issue, and the challenge was spent whether or not the proof verified.
			const uint64_t provedAtMs = wire.nowMs;
			const NetH4Challenge* known = LastOf<NetH4Challenge>(wire.Delivered(54));
			NetH4Proof bad;
			bad.txId = known->txId;
			bad.epoch = record.epoch;
			bad.stableSeat = 0;
			bad.holderGeneration = 1;
			bad.clientNonce = Ramp<16>(0xB0);
			bad.mac = Ramp<32>(0xC0);
			if (!wire.SendRaw(54, bad, &error)) {
				return Fail(error);
			}
			if (wire.host.GetAdmission().GetOutstandingChallengeCount() != 0) {
				return Fail("the challenge survived the attempt that spent it");
			}
			wire.nowMs = provedAtMs + NetReconnectAdmission::c_DenialReleaseMs - 1;
			wire.DrainHostOutbound();
			if (CountOf<NetJoinRejected>(wire.Delivered(54)) != 0) {
				return Fail("the bad proof was refused early");
			}
			wire.nowMs = provedAtMs + NetReconnectAdmission::c_DenialReleaseMs;
			wire.DrainHostOutbound();
			if (CountOf<NetJoinRejected>(wire.Delivered(54)) != 1) {
				return Fail("the bad proof was not refused on the same schedule");
			}
			if (!(*LastOf<NetJoinRejected>(wire.Delivered(54)) == *reference)) {
				return Fail("a bad proof reads differently from an unknown seat");
			}
			// The seat is untouched by every one of those attempts.
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(0, holder, generation, incarnation) || generation != 1 || incarnation != 1) {
				return Fail("a refused attempt disturbed the seat");
			}
			return 0;
		}

		int TestChallengeAbuse() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint player;
			player.connection = 61;
			ConfigureEndpoint(player, "abuse", &unixNow);
			wire.Add(&player);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, player, record, unixNow, &error) != 0) {
				return Fail("could not seat the player: " + error);
			}
			wire.host.SetLiveMatch(true);
			wire.ClearDelivered();

			const auto reclaim = [&record](uint8_t txSeed) {
				NetH4Reclaim message;
				message.txId = Ramp<16>(txSeed);
				message.epoch = record.epoch;
				message.stableSeat = record.stableSeat;
				message.holderGeneration = record.holderGeneration;
				message.identity = MakeIdentity();
				message.displayName = "abuse";
				return message;
			};
			const auto proveWith = [&record](const NetH4Challenge& challenge, uint16_t seat, uint32_t generation, const NetAuthBytes16& epoch) {
				NetH4Transcript transcript;
				transcript.domain = NetH4ProofDomain::Reclaim;
				transcript.protocolVersion = NetProtocol::c_Version;
				transcript.epoch = epoch;
				transcript.stableSeat = seat;
				transcript.holderGeneration = generation;
				transcript.challenge = challenge.challenge;
				transcript.clientNonce = Ramp<16>(0xD0);
				NetH4Proof proof;
				proof.txId = challenge.txId;
				proof.epoch = epoch;
				proof.stableSeat = seat;
				proof.holderGeneration = generation;
				proof.clientNonce = transcript.clientNonce;
				(void)NetH4ComputeProof(record.credential, transcript, proof.mac);
				return proof;
			};

			// A cross-context proof: the right challenge answered for another seat.
			if (!wire.SendRaw(61, reclaim(0x70), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			const NetH4Challenge crossChallenge = *LastOf<NetH4Challenge>(wire.Delivered(61));
			if (!wire.SendRaw(61, proveWith(crossChallenge, 1, record.holderGeneration, record.epoch), &error)) {
				return Fail(error);
			}
			wire.nowMs += NetReconnectAdmission::c_DenialReleaseMs;
			wire.DrainHostOutbound();
			if (CountOf<NetH4JoinCommitted>(wire.Delivered(61)) != 0 || CountOf<NetJoinRejected>(wire.Delivered(61)) != 1) {
				return Fail("a cross-seat proof was not refused");
			}
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (wire.host.GetSeatHolder(1, holder, generation, incarnation)) {
				return Fail("a cross-seat proof committed the other seat");
			}

			// The honest proof commits, and the challenge it spent cannot be answered a second time.
			wire.ClearDelivered();
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!wire.SendRaw(61, reclaim(0x71), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			const NetH4Challenge live = *LastOf<NetH4Challenge>(wire.Delivered(61));
			const NetH4Proof good = proveWith(live, record.stableSeat, record.holderGeneration, record.epoch);
			if (!wire.SendRaw(61, good, &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (CountOf<NetH4JoinCommitted>(wire.Delivered(61)) != 1) {
				return Fail("the honest proof did not commit");
			}
			NetH4Proof replayed = good;
			replayed.txId = Ramp<16>(0x72);
			if (!wire.SendRaw(61, replayed, &error)) {
				return Fail(error);
			}
			wire.nowMs += NetReconnectAdmission::c_DenialReleaseMs;
			wire.DrainHostOutbound();
			if (CountOf<NetJoinRejected>(wire.Delivered(61)) != 1) {
				return Fail("a replayed proof under a fresh transaction was not refused");
			}

			// An expired challenge cannot be answered at all.
			wire.ClearDelivered();
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!wire.SendRaw(62, reclaim(0x73), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			const NetH4Challenge expiring = *LastOf<NetH4Challenge>(wire.Delivered(62));
			wire.nowMs += NetReconnectAdmission::c_ChallengeLifetimeMs + 1;
			if (!wire.SendRaw(62, proveWith(expiring, record.stableSeat, record.holderGeneration, record.epoch), &error)) {
				return Fail(error);
			}
			wire.nowMs += NetReconnectAdmission::c_DenialReleaseMs;
			wire.DrainHostOutbound();
			if (CountOf<NetJoinRejected>(wire.Delivered(62)) != 1 || CountOf<NetH4JoinCommitted>(wire.Delivered(62)) != 0) {
				return Fail("an expired challenge was still answerable");
			}
			return 0;
		}

		// §6: two connections hold the same valid ticket, so the newly proven one replaces the old, and the
		// old one's delayed traffic and later timeout must not touch the seat.
		int TestSingleActiveIncarnation() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint first;
			first.connection = 71;
			ConfigureEndpoint(first, "fence", &unixNow);
			wire.Add(&first);
			NetH4TicketRecord record;
			if (!first.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the seeding join did not settle: " + error);
			}
			if (first.store.Load(unixNow, record, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail(error);
			}
			wire.host.SetLiveMatch(true);

			// A second process holding the same ticket proves it while the first is still connected.
			Endpoint second;
			second.connection = 72;
			ConfigureEndpoint(second, "fence-second", &unixNow);
			wire.Add(&second);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!second.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the second holder's reclaim did not settle: " + error);
			}
			if (second.client.GetState() != NetH4ClientState::Joined || second.client.GetIncarnation() != 2) {
				return Fail("the newly proven connection did not replace the old one");
			}
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(0, holder, generation, incarnation) || holder != second.connection || incarnation != 2) {
				return Fail("the seat did not move to the newer incarnation");
			}
			if (!wire.host.IsFenced(first.connection)) {
				return Fail("the superseded transport was not fenced");
			}

			// A delayed leave from the old transport must not reach the seat.
			const uint32_t leavesBefore = wire.host.GetStats().seatsClosedByLeave;
			const uint32_t fencedBefore = wire.host.GetStats().fencedPackets;
			NetH4LeaveRequest stale;
			stale.txId = Ramp<16>(0x80);
			stale.epoch = record.epoch;
			stale.stableSeat = record.stableSeat;
			stale.holderGeneration = record.holderGeneration;
			wire.host.CountFencedPacket();
			if (wire.host.GetStats().fencedPackets != fencedBefore + 1) {
				return Fail("the fenced packet was not counted");
			}
			// Even if a fenced packet did reach the plane, the seat only answers its active incarnation.
			if (!wire.SendRaw(first.connection, stale, &error)) {
				return Fail(error);
			}
			if (wire.host.GetStats().seatsClosedByLeave != leavesBefore) {
				return Fail("a superseded transport closed the seat");
			}
			if (!wire.host.GetSeatHolder(0, holder, generation, incarnation) || holder != second.connection) {
				return Fail("a fenced packet moved the seat");
			}

			// The old transport's later timeout must not evict the live holder.
			if (wire.host.NotifyDisconnect(first.connection, 200) != NetH4DisconnectOutcome::Fenced) {
				return Fail("the superseded transport's timeout was not fenced");
			}
			if (!wire.host.GetSeatHolder(0, holder, generation, incarnation) || holder != second.connection || incarnation != 2) {
				return Fail("a fenced timeout evicted the seat");
			}
			if (wire.host.GetStats().fencedDisconnects != 1 || wire.host.GetStats().seatsDropped != 0) {
				return Fail("the fenced timeout was counted as a seat drop");
			}

			// The live holder's own disconnect IS a seat drop.
			if (wire.host.NotifyDisconnect(second.connection, 300) != NetH4DisconnectOutcome::SeatDropped) {
				return Fail("the live holder's disconnect was not a seat drop");
			}
			if (wire.host.GetStats().seatsDropped != 1) {
				return Fail("the real drop was not counted");
			}
			return 0;
		}

		int TestCleanLeaveAndAmbiguity() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint player;
			player.connection = 81;
			ConfigureEndpoint(player, "leave", &unixNow);
			wire.Add(&player);
			if (!player.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the seeding join did not settle: " + error);
			}
			NetH4TicketRecord record;
			if (player.store.Load(unixNow, record, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail(error);
			}

			// An unacknowledged leave is an ambiguous loss: the record stays and the link closes.
			{
				Endpoint quitter;
				quitter.connection = 82;
				ConfigureEndpoint(quitter, "leave-lost", &unixNow);
				if (!quitter.store.Store(record, &error)) {
					return Fail("could not seed the ambiguous-loss record: " + error);
				}
				if (!quitter.client.BeginReclaim(record, 0, &error)) {
					return Fail(error);
				}
				quitter.client.TakeOutbound();
				if (!quitter.client.BeginLeave(0, &error)) {
					return Fail(error);
				}
				quitter.client.TakeOutbound();
				for (uint64_t now = 0; now <= NetReconnectClient::c_LeaveAckBudgetMs; now += 250) {
					quitter.client.Tick(now);
					quitter.client.TakeOutbound();
				}
				if (!quitter.client.WantsLinkClosed()) {
					return Fail("an unacknowledged leave did not ask to close the link");
				}
				if (quitter.client.GetState() == NetH4ClientState::Left) {
					return Fail("an unacknowledged leave was treated as a clean one");
				}
				if (!quitter.store.HasRecord()) {
					return Fail("an ambiguous loss cleared the recovery record");
				}
				if (quitter.client.GetStats().unacknowledgedLeaves != 1) {
					return Fail("the unacknowledged leave was not counted");
				}
			}

			// The acknowledged leave clears the ticket and closes the seat.
			wire.ClearDelivered();
			if (!player.client.BeginLeave(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the clean leave did not settle: " + error);
			}
			if (player.client.GetState() != NetH4ClientState::Left) {
				return Fail("the acknowledged leave did not complete");
			}
			if (player.store.HasRecord()) {
				return Fail("the acknowledged leave did not clear the ticket");
			}
			if (!wire.host.IsSeatClosed(record.stableSeat)) {
				return Fail("the leave did not close the seat");
			}
			if (wire.registry.GetActiveGeneration(record.stableSeat) != 0) {
				return Fail("the leave did not revoke the holder generation");
			}

			// A retransmitted request replays the same ack rather than failing.
			const NetH4LeaveAck firstAck = *LastOf<NetH4LeaveAck>(wire.Delivered(player.connection));
			const uint32_t replaysBefore = wire.host.GetStats().replayedResults;
			NetH4LeaveRequest again;
			again.txId = firstAck.txId;
			again.epoch = record.epoch;
			again.stableSeat = record.stableSeat;
			again.holderGeneration = record.holderGeneration;
			if (!wire.SendRaw(player.connection, again, &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (wire.host.GetStats().replayedResults != replaysBefore + 1 ||
			    !(*LastOf<NetH4LeaveAck>(wire.Delivered(player.connection)) == firstAck)) {
				return Fail("a retransmitted leave did not replay its ack");
			}

			// The old ticket cannot reclaim the closed seat.
			wire.ClearDelivered();
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			NetH4Reclaim stale;
			stale.txId = Ramp<16>(0x90);
			stale.epoch = record.epoch;
			stale.stableSeat = record.stableSeat;
			stale.holderGeneration = record.holderGeneration;
			stale.identity = MakeIdentity();
			stale.displayName = "left";
			if (!wire.SendRaw(83, stale, &error)) {
				return Fail(error);
			}
			wire.nowMs += NetReconnectAdmission::c_DenialReleaseMs;
			wire.DrainHostOutbound();
			if (CountOf<NetH4JoinCommitted>(wire.Delivered(83)) != 0 || CountOf<NetJoinRejected>(wire.Delivered(83)) != 1) {
				return Fail("a ticket for a cleanly left seat still reclaimed it");
			}
			return 0;
		}

		// The census the drop-time ledger records; a file-scope table so the ownership source can be a
		// plain function pointer the way the match runner will supply one.
		std::vector<NetH4LedgerActor> g_Census;

		std::vector<NetH4LedgerActor> CensusSource(void*) {
			return g_Census;
		}

		int TestLedgerAndReseat() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			// Seat 0's peer owns three actors on team 1; another peer owns one on team 2.
			g_Census = {
			    {101, 1, 2, true},
			    {102, 1, 2, true},
			    {103, 1, 2, true},
			    {201, 2, 3, true},
			};
			if (NetReconnectLedger::CollectOwnedActorUIDs(g_Census, 2) != std::vector<int64_t>{101, 102, 103}) {
				return Fail("the drop census did not collect exactly the peer's actors, in order");
			}

			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			wire.host.SetDropOwnershipSource(&CensusSource, nullptr);
			Endpoint player;
			player.connection = 91;
			ConfigureEndpoint(player, "ledger", &unixNow);
			wire.Add(&player);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, player, record, unixNow, &error) != 0) {
				return Fail("could not seat the player: " + error);
			}
			const NetH4SeatOwnership* ledgered = wire.host.GetLedger().Find(0);
			if (ledgered == nullptr || ledgered->actorUIDs != std::vector<int64_t>{101, 102, 103} ||
			    ledgered->peerId != MakeSeatTable()[0].lockstepPeerId || ledgered->team != 1 || ledgered->droppedAtFrame != 100) {
				return Fail("the drop did not record the seat's ownership");
			}

			// While the holder is away one actor dies and one leaves the team; neither comes back.
			g_Census = {
			    {101, 1, 0, true},
			    {102, 1, 0, false},
			    {103, 2, 0, true},
			    {201, 2, 3, true},
			};
			wire.host.SetLiveMatch(true);
			Endpoint returner;
			returner.connection = 92;
			ConfigureEndpoint(returner, "ledger", &unixNow);
			wire.Add(&returner);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the reclaim did not settle: " + error);
			}
			const std::vector<NetGameReseat> reseats = wire.host.TakePendingReseats();
			if (reseats.size() != 1) {
				return Fail("the committed reclaim did not produce exactly one reseat");
			}
			// The reseat names the LOCKSTEP id, which is not the session id the commit handed back.
			if (reseats.front().team != 1 || reseats.front().newOwnerPeerId != MakeSeatTable()[0].lockstepPeerId ||
			    reseats.front().newOwnerPeerId == MakeSeatTable()[0].peerId) {
				return Fail("the reseat did not address the returner's own seat");
			}
			if (reseats.front().actorUIDs != std::vector<int64_t>{101}) {
				return Fail("the reseat did not restore exactly the surviving, still-on-team actors");
			}

			// The reseat rides the lockstep wire, so it must survive the frame codec byte for byte.
			{
				NetLockstepFrame frame;
				frame.senderPeerId = 1;
				frame.targetFrame = 42;
				frame.commands.push_back(NetGameCommand{1, reseats.front()});
				std::vector<uint8_t> bytes;
				NetLockstepError encodeError;
				if (!NetLockstepCodec::Encode(NetLockstepPacket{frame}, bytes, &encodeError)) {
					return Fail("the reseat command did not encode: " + encodeError.message);
				}
				const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes);
				if (!decoded.ok) {
					return Fail("the reseat command did not decode: " + decoded.error.message);
				}
				const NetLockstepFrame* roundtripped = std::get_if<NetLockstepFrame>(&decoded.packet.payload);
				if (roundtripped == nullptr || roundtripped->commands.size() != 1) {
					return Fail("the decoded frame lost the reseat command");
				}
				const NetGameReseat* decodedReseat = std::get_if<NetGameReseat>(&roundtripped->commands.front().payload);
				if (decodedReseat == nullptr || !(*decodedReseat == reseats.front())) {
					return Fail("the reseat command did not survive the codec");
				}
			}

			// The per-seat cap bounds a degenerate or hostile scene.
			{
				NetReconnectLedger ledger;
				std::vector<int64_t> many;
				for (int64_t uid = 0; uid < static_cast<int64_t>(NetReconnectLedger::c_MaxActorsPerSeat) + 7; ++uid) {
					many.push_back(uid + 1);
				}
				ledger.RecordDrop(3, 4, 1, 7, many);
				const NetH4SeatOwnership* capped = ledger.Find(3);
				if (capped == nullptr || capped->actorUIDs.size() != NetReconnectLedger::c_MaxActorsPerSeat || ledger.GetActorsTruncated() != 7) {
					return Fail("the ledger did not bound a seat's actor list");
				}
				ledger.RecordDrop(3, 4, 1, 9, {5, 6});
				if (ledger.Size() != 1 || ledger.Find(3)->actorUIDs != std::vector<int64_t>{5, 6} || ledger.Find(3)->droppedAtFrame != 9) {
					return Fail("a later drop did not replace the seat's record");
				}
				ledger.ClearSeat(3);
				if (ledger.Find(3) != nullptr) {
					return Fail("the ledger entry outlived its seat");
				}
			}

			// A clean leave clears the seat's ledger entry along with its credential.
			wire.ClearDelivered();
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!returner.client.BeginLeave(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the leave did not settle: " + error);
			}
			if (wire.host.GetLedger().Find(0) != nullptr) {
				return Fail("the leave left the seat ownership ledger behind");
			}
			return 0;
		}

		// The ticket survives a rematch round: the seat table is re-published, the epoch is not rearmed.
		int TestRejoinAcrossRematch() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint player;
			player.connection = 111;
			ConfigureEndpoint(player, "rematch", &unixNow);
			wire.Add(&player);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, player, record, unixNow, &error) != 0) {
				return Fail("could not seat the player: " + error);
			}

			wire.host.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			wire.host.SetLiveMatch(false);
			if (!(wire.registry.GetEpoch() == record.epoch)) {
				return Fail("the rematch rearmed the epoch");
			}

			// A previously claimed seat stays protected for the whole hosted session, so a ticketless
			// client lands on the next never-held seat instead.
			Endpoint stranger;
			stranger.connection = 112;
			ConfigureEndpoint(stranger, "rematch-stranger", &unixNow);
			wire.Add(&stranger);
			if (!stranger.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the stranger join did not settle: " + error);
			}
			if (stranger.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the stranger could not fill a never-held seat");
			}
			NetH4TicketRecord strangerRecord;
			if (stranger.store.Load(unixNow, strangerRecord, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail(error);
			}
			if (strangerRecord.stableSeat == record.stableSeat) {
				return Fail("a ticketless client took a previously claimed seat");
			}

			// The original ticket still reclaims its own seat after the rematch.
			Endpoint returner;
			returner.connection = 113;
			ConfigureEndpoint(returner, "rematch", &unixNow);
			wire.Add(&returner);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the rematch reclaim did not settle: " + error);
			}
			if (returner.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the ticket did not carry across the rematch");
			}
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(record.stableSeat, holder, generation, incarnation) || holder != returner.connection) {
				return Fail("the rematch reclaim did not land on the original seat");
			}
			return 0;
		}

		NetIdentityManifest MakeManifest() {
			NetIdentityManifest manifest;
			manifest.gameVersion = "7.0.0-test";
			manifest.networkProtocolVersion = NetProtocol::c_Version;
			manifest.controllerFrameVersion = ControllerFrame::c_Version;
			manifest.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
			manifest.buildId = "stage2-h4a2";
			manifest.platform = "test";
			manifest.deterministicConfigHash = MakeHash(1);
			manifest.moduleManifestHash = MakeHash(33);
			manifest.sessionRulesHash = MakeHash(65);
			manifest.sessionIdentityHash = MakeHash(97);
			manifest.hasUserdataModules = false;
			return manifest;
		}

		NetSessionConfig MakeSessionConfig(uint16_t port, uint64_t nonce, const std::string& name) {
			NetSessionConfig config;
			config.localIdentity = MakeManifest();
			config.displayName = name;
			config.port = port;
			config.sessionId = 0x5000000000000000ULL + port;
			config.localNonce = nonce;
			config.maxPeers = 2;
			config.heartbeatIntervalMs = 50;
			config.timeoutMs = 5000;
			return config;
		}

		// The wiring itself: an admission transaction that rides two real NetSessions, commits the seat
		// onto the session's peer list, and a hosted-session end the client is allowed to act on.
		int TestSessionWiring() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}

			const uint16_t port = 42101;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetSession host;
			NetSession client;
			NetSeatAuthRegistry registry;
			registry.BeginHostedSession();
			NetReconnectHost admission;
			admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
			admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			NetReconnectTicketStore store;
			store.SetPath(StorePath("wiring"));
			NetReconnectClient reconnect;
			uint64_t unixNow = 1'700'000'000'000ULL;
			reconnect.Configure(&store, MakeIdentity(), "Player");
			reconnect.SetUnixClock(&FixedUnixClock, &unixNow);
			host.SetReconnectHost(&admission);
			client.SetReconnectClient(&reconnect);

			if (!host.StartHost(hostTransport, MakeSessionConfig(port, 101, "Host"), &error) ||
			    !client.StartClient(clientTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not start the wired pair: " + error);
			}
			uint64_t nowMs = 0;
			const auto drive = [&](uint64_t untilMs) {
				for (; nowMs <= untilMs; nowMs += 10) {
					host.Tick(nowMs);
					client.Tick(nowMs);
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
				}
			};
			// With a plane attached the transaction is part of the handshake now: the session starts it
			// on JoinAccepted and holds Ready back until the commit, so nothing here starts it by hand.
			drive(600);
			if (!host.IsReady() || !client.IsReady()) {
				return Fail("the wired pair did not reach the ready state");
			}
			if (reconnect.GetState() != NetH4ClientState::Joined) {
				return Fail("the admission transaction did not commit through NetSession");
			}
			if (reconnect.GetStats().requestsSent != 1) {
				return Fail("the handshake ran more than one admission transaction");
			}
			if (!store.HasRecord()) {
				return Fail("the wired join left no durable ticket");
			}
			// P4: the committed peer sits on the seat's own recorded peer id.
			const std::vector<NetSessionPeerInfo> readyPeers = host.GetReadyPeers();
			if (readyPeers.size() != 1 || readyPeers.front().assignedPeerId != MakeSeatTable()[0].peerId) {
				return Fail("the committed seat did not become a ready peer on its own peer id");
			}
			if (host.GetStats().admissionMessages == 0) {
				return Fail("the host counted no admission messages");
			}
			const std::string report = host.BuildReportJson();
			for (const char* key : {"\"admission\"", "\"provisional_seats_committed\": 1", "\"unbound_connection_faults\"",
			                        "\"unauthenticated_connections_refused\"", "\"fenced_packets\"", "\"fenced_disconnects\"",
			                        "\"seats_closed_by_leave\"", "\"ledger_drops_recorded\"", "\"reseats_issued\""}) {
				if (report.find(key) == std::string::npos) {
					return Fail(std::string("the session report is missing ") + key);
				}
			}
			// Secrets stay off the report.
			NetH4TicketRecord record;
			if (store.Load(unixNow, record, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail(error);
			}
			if (report.find(HexOf(record.credential.data(), record.credential.size(), false)) != std::string::npos ||
			    report.find(HexOf(record.epoch.data(), record.epoch.size(), false)) != std::string::npos) {
				return Fail("the session report leaked auth material");
			}

			// The confirmed hosted-session end - and nothing else - lets the client delete its record.
			host.EndHostedSession("the host closed the session");
			drive(nowMs + 200);
			if (reconnect.GetStats().confirmedSessionEnds != 1) {
				return Fail("the client did not see the confirmed session end");
			}
			if (store.HasRecord()) {
				return Fail("a confirmed session end did not clear the recovery record");
			}
			return 0;
		}

		// An ordinary transport disconnect and a session timeout are NOT a confirmed session end.
		int TestSessionEndIsTheOnlySignal() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			NetReconnectTicketStore store;
			store.SetPath(StorePath("signal"));
			NetH4TicketRecord record;
			record.recordVersion = NetReconnectTicketStore::c_RecordVersion;
			record.epoch = Ramp<16>(0x11);
			record.stableSeat = 0;
			record.holderGeneration = 1;
			record.credential = Ramp<32>(0x44);
			record.hostSessionId = 7;
			record.issuedAtUnixMs = 1'700'000'000'000ULL;
			if (!store.Store(record, &error)) {
				return Fail(error);
			}
			NetReconnectClient client;
			uint64_t unixNow = record.issuedAtUnixMs;
			client.Configure(&store, MakeIdentity(), "Player");
			client.SetUnixClock(&FixedUnixClock, &unixNow);
			if (!client.BeginReclaim(record, 0, &error)) {
				return Fail(error);
			}
			client.TakeOutbound();

			// Neither an ambiguous loss nor a heartbeat timeout is a confirmed session end.
			client.NotifyAmbiguousLoss();
			if (!store.HasRecord() || client.GetStats().ambiguousLosses != 1) {
				return Fail("an ambiguous loss deleted the recovery record");
			}
			client.Tick(NetReconnectClient::c_LeaveAckBudgetMs * 4);
			client.TakeOutbound();
			if (!store.HasRecord()) {
				return Fail("a stalled reclaim deleted the recovery record");
			}
			client.NotifyConfirmedSessionEnd();
			if (store.HasRecord()) {
				return Fail("a confirmed session end left the recovery record behind");
			}
			return 0;
		}

		class ScriptedLobbyTransport final : public INetTransport {
		public:
			struct SentPacket {
				NetPeerId peerId = c_InvalidNetPeerId;
				std::vector<uint8_t> bytes;
			};

			bool StartHost(uint16_t, std::string*) override { return true; }
			bool Connect(const std::string&, uint16_t, std::string* error) override {
				if (error) *error = "scripted transport does not connect";
				return false;
			}
			bool Send(NetPeerId peerId, NetTransportLane, const std::vector<uint8_t>& bytes, std::string*) override {
				sentPackets.push_back({peerId, bytes});
				return true;
			}
			void Disconnect(NetPeerId peerId, const std::string&) override { disconnected.push_back(peerId); }
			void Stop() override { events.clear(); }
			std::vector<NetTransportEvent> PollEvents() override {
				std::vector<NetTransportEvent> result = std::move(events);
				events.clear();
				return result;
			}
			void Push(NetTransportEvent event) { events.push_back(std::move(event)); }

			std::vector<SentPacket> sentPackets;
			std::vector<NetPeerId> disconnected;

		private:
			std::vector<NetTransportEvent> events;
		};

		// §0/§9a on the third plane: the rematch/resync lobby must survive everything an unbound joiner
		// can do to it, and the players already in the round must not notice.
		int TestLobbyAdmissionIsolation() {
			std::string error;
			ScriptedLobbyTransport transport;
			NetLobbySession lobby;
			NetLobbySessionConfig config;
			config.host = true;
			config.localPeerId = 1;
			config.remoteTransportPeerIds = {{2, 200}};
			config.matchConfig = NetMatchConfigUtil::MakeDefault(0x4831ULL);
			config.startFrame = 1;
			config.autoReady = true;
			config.autoStart = false;
			config.displayName = "Host";
			if (!lobby.Start(transport, config, &error)) {
				return Fail("the lobby round did not start: " + error);
			}
			lobby.Tick(0);
			const NetLobbyState stateBefore = lobby.GetState();
			const NetLobbyStats statsBefore = lobby.GetStats();
			const NetHash32 configHashBefore = lobby.GetMatchConfigHash();
			const bool remoteReadyBefore = lobby.IsRemoteReady(2);
			const size_t disconnectsBefore = transport.disconnected.size();
			if (stateBefore == NetLobbyState::Failed || stateBefore == NetLobbyState::Rejected) {
				return Fail("the lobby round was not healthy before the attack");
			}

			const NetPeerId unbound = 900;
			// Malformed, oversized, random and old-version packets, then an abrupt loss on both event types.
			std::vector<uint8_t> oversized(NetLockstepCodec::c_MaxPayloadBytes + 4096, 0x5AU);
			std::vector<uint8_t> oldVersion;
			{
				NetMessage message;
				message.sequence = 1;
				message.payload = NetHeartbeat{1, 2, 3};
				NetProtocolError encodeError;
				if (!NetProtocol::Encode(message, oldVersion, &encodeError)) {
					return Fail("could not build the old-wire packet: " + encodeError.message);
				}
				// An envelope from a build whose header version we do not speak.
				oldVersion[4] = 0xFFU;
				oldVersion[5] = 0xFFU;
			}
			transport.Push({NetTransportEventType::PacketReceived, unbound, NetTransportLane::ControlReliable, {0x01U, 0x02U, 0x03U}, ""});
			transport.Push({NetTransportEventType::PacketReceived, unbound, NetTransportLane::ControlReliable, oversized, ""});
			transport.Push({NetTransportEventType::PacketReceived, unbound, NetTransportLane::ControlReliable, {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U, 0x11U, 0x22U, 0x33U}, ""});
			transport.Push({NetTransportEventType::PacketReceived, unbound, NetTransportLane::ControlReliable, oldVersion, ""});
			transport.Push({NetTransportEventType::PeerDisconnected, unbound, NetTransportLane::ControlReliable, {}, "gone"});
			// The transport cannot attribute these, so they arrive with no peer at all.
			transport.Push({NetTransportEventType::ConnectionFailed, c_InvalidNetPeerId, NetTransportLane::ControlReliable, {}, "connection failed"});
			transport.Push({NetTransportEventType::TransportError, unbound, NetTransportLane::ControlReliable, {}, "transport error"});
			lobby.Tick(10);

			if (lobby.GetState() != stateBefore) {
				return Fail("an unbound joiner moved the lobby round out of its state");
			}
			if (lobby.IsFailed() || lobby.IsRejected()) {
				return Fail("an unbound joiner aborted the lobby round: " + lobby.GetFailureReason());
			}
			if (lobby.IsRemoteReady(2) != remoteReadyBefore || !(lobby.GetMatchConfigHash() == configHashBefore)) {
				return Fail("an unbound joiner disturbed the committed remote");
			}
			const NetLobbyStats statsAfter = lobby.GetStats();
			if (statsAfter.malformedMessages != statsBefore.malformedMessages || statsAfter.messagesReceived != statsBefore.messagesReceived) {
				return Fail("an unbound joiner's garbage was counted as lobby traffic");
			}
			if (statsAfter.unboundConnectionFaults != 2 || statsAfter.unboundDisconnects != 1) {
				return Fail("the unbound faults were not counted: " + std::to_string(statsAfter.unboundConnectionFaults) + "/" + std::to_string(statsAfter.unboundDisconnects));
			}
			if (transport.disconnected.size() != disconnectsBefore) {
				return Fail("an unattributable fault disconnected somebody");
			}

			// Positive control: a committed remote's fault still removes it, so the fix did not blanket-ignore.
			transport.Push({NetTransportEventType::TransportError, 200, NetTransportLane::ControlReliable, {}, "committed remote fault"});
			lobby.Tick(20);
			if (lobby.GetStats().unboundConnectionFaults != 2) {
				return Fail("a committed remote's fault was ignored as unbound");
			}
			if (std::find(transport.disconnected.begin(), transport.disconnected.end(), static_cast<NetPeerId>(200)) == transport.disconnected.end()) {
				return Fail("a committed remote's fault did not close its connection");
			}
			// It really is gone: the next fault on that transport is now an unbound one.
			transport.Push({NetTransportEventType::TransportError, 200, NetTransportLane::ControlReliable, {}, "after removal"});
			lobby.Tick(30);
			if (lobby.GetStats().unboundConnectionFaults != 3) {
				return Fail("the committed remote was not actually removed by its fault");
			}
			return 0;
		}
		// Without a provider nothing may be written and nothing may be trusted: no ticket, no reclaim.
		int TestStoreFailsClosed() {
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			NetReconnectTicketStore store;
			store.SetPath(StorePath("failclosed"));
			NetH4TicketRecord record;
			record.recordVersion = NetReconnectTicketStore::c_RecordVersion;
			record.epoch = Ramp<16>(0x22);
			record.stableSeat = 0;
			record.holderGeneration = 1;
			record.credential = Ramp<32>(0x55);
			record.issuedAtUnixMs = 1'700'000'000'000ULL;

#ifndef CCCP_WITH_GNS
			// The build's own default provider, with no override installed at all.
			if (GetNetAuthCrypto().IsRealCrypto()) {
				return Fail("the GNS-less default provider must fail closed");
			}
			if (store.Store(record, &error) || store.HasRecord()) {
				return Fail("a ticket was written without a crypto provider");
			}
#endif
			{
				ScriptedAuthCrypto crypto;
				ScopedTestCrypto scope(&crypto);
				if (!store.Store(record, &error)) {
					return Fail("the store refused a record with a working provider: " + error);
				}
				crypto.failHmac = true;
				NetH4TicketRecord loaded;
				if (store.Load(record.issuedAtUnixMs, loaded, &error) != NetH4TicketLoadResult::Corrupt) {
					return Fail("a record loaded even though its integrity could not be checked");
				}
				NetH4TicketRecord second = record;
				second.holderGeneration = 2;
				if (store.Store(second, &error)) {
					return Fail("a record was written even though it could not be macced");
				}
				crypto.failHmac = false;
				if (store.Load(record.issuedAtUnixMs, loaded, &error) != NetH4TicketLoadResult::Loaded || !(loaded == record)) {
					return Fail("the failed write did not leave the previous record intact");
				}
			}
			return 0;
		}

		// The seat table a real match config produces. The commit hands back a SESSION id and the ledger
		// names a LOCKSTEP id; the host's own seat is never offered, and a CPU slot never is either.
		int TestSeatTableFromMatchConfig() {
			NetMatchConfig config = NetMatchConfigUtil::MakeDefault(0x4242ULL);
			config.mode = NetMatchMode::PvPvE;
			config.peerCount = 3;
			config.players.clear();
			for (uint8_t peerId = 1; peerId <= 3; ++peerId) {
				NetMatchPlayerSlot slot;
				slot.peerId = peerId;
				slot.team = static_cast<uint8_t>(peerId - 1);
				slot.displayName = "P" + std::to_string(peerId);
				config.players.push_back(slot);
			}
			NetMatchPlayerSlot cpu;
			cpu.peerId = 0;
			cpu.team = 3;
			cpu.cpu = true;
			cpu.displayName = "CPU";
			config.players.push_back(cpu);

			const std::vector<NetH4Seat> seats = NetH4BuildSeatTable(config);
			if (seats.size() != 4) {
				return Fail("the seat table lost a slot");
			}
			for (size_t index = 0; index < seats.size(); ++index) {
				if (seats[index].stableSeat != static_cast<uint16_t>(index)) {
					return Fail("the stable seat is not the slot index");
				}
			}
			if (!seats[0].local || seats[1].local || seats[2].local || seats[3].local) {
				return Fail("the host's own seat was not the only local one");
			}
			if (!seats[3].cpu || seats[3].lockstepPeerId != 0) {
				return Fail("the CPU slot was not carried as one");
			}
			for (size_t index = 0; index < 3; ++index) {
				if (seats[index].lockstepPeerId != config.players[index].peerId) {
					return Fail("a seat lost its lockstep peer id");
				}
				if (seats[index].peerId != static_cast<uint8_t>(config.players[index].peerId - 1)) {
					return Fail("a seat's session peer id is not one below its lockstep id");
				}
				if (seats[index].team != static_cast<int32_t>(config.players[index].team)) {
					return Fail("a seat lost its team");
				}
			}

			// A joiner must never be handed the host's own seat or the CPU's.
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1700000000000ULL;
			Wire wire;
			wire.registry.BeginHostedSession();
			wire.host.Configure(&wire.registry, 0x4242ULL, MakeIdentity());
			wire.host.SetSeatTable(seats, config.mode);
			wire.host.SetLiveMatch(false);
			Endpoint joiner;
			joiner.connection = 71;
			ConfigureEndpoint(joiner, "seattable", &unixNow);
			wire.Add(&joiner);
			if (!joiner.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the joiner did not settle: " + error);
			}
			if (joiner.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the joiner never committed a seat");
			}
			if (joiner.client.GetRecord().stableSeat == seats[0].stableSeat) {
				return Fail("a joiner was handed the host's own seat");
			}
			if (joiner.client.GetAssignedPeerId() != seats[1].peerId) {
				return Fail("the joiner did not land on the first joinable seat's session id");
			}
			return 0;
		}

		// §10's probe. Two arms, because the answer differs by whether the envelope lets us speak the
		// peer's version: an unknown message type under the SHARED header version gets an explicit,
		// decodable rejection; a header version this build cannot write gets a counted best-effort
		// disconnect and NOT an undecodable reply. Both leave the live players untouched.
		int TestOldWireProbe() {
			std::string error;
			// The mechanism first: we stamp the version we were asked for, and refuse any other.
			{
				NetMessage rejection;
				rejection.sequence = 7;
				rejection.payload = NetJoinRejected{NetRejectReason::ProtocolMismatch, "old wire", "protocol_version", "1", "2"};
				std::vector<uint8_t> bytes;
				if (!NetProtocol::EncodeAtVersion(rejection, NetProtocol::c_Version, bytes)) {
					return Fail("a rejection would not encode at the version this build speaks");
				}
				uint16_t stamped = 0;
				if (!NetProtocol::PeekHeaderVersion(bytes.data(), bytes.size(), stamped) || stamped != NetProtocol::c_Version) {
					return Fail("the rejection did not carry the version it was stamped at");
				}
				if (!NetProtocol::Decode(bytes).ok) {
					return Fail("the version-stamped rejection did not decode");
				}
				std::vector<uint8_t> refused;
				if (NetProtocol::EncodeAtVersion(rejection, static_cast<uint16_t>(NetProtocol::c_Version + 1), refused) ||
				    NetProtocol::CanEncodeAtVersion(static_cast<uint16_t>(NetProtocol::c_Version + 1))) {
					return Fail("a rejection was encoded at a version whose schema this build cannot write");
				}
				if (!refused.empty()) {
					return Fail("the refused encode left bytes behind");
				}
			}

			const uint16_t port = 42134;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetSession host;
			NetSession client;
			if (!host.StartHost(hostTransport, MakeSessionConfig(port, 101, "Host"), &error) ||
			    !client.StartClient(clientTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not start the pair the old-wire peer joins: " + error);
			}
			LoopbackTransport oldWireTransport;
			LoopbackTransport futureWireTransport;
			uint64_t nowMs = 0;
			const auto drive = [&](uint64_t untilMs) {
				for (; nowMs <= untilMs; nowMs += 10) {
					host.Tick(nowMs);
					client.Tick(nowMs);
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
					oldWireTransport.AdvanceTimeMs(10);
					futureWireTransport.AdvanceTimeMs(10);
				}
			};
			drive(300);
			if (!host.IsReady() || host.GetReadyPeerCount() != 1) {
				return Fail("the live pair never settled before the old-wire peer arrived");
			}
			const uint32_t readyBefore = host.GetReadyPeerCount();

			const auto envelope = [](uint16_t version, uint16_t messageType) {
				std::vector<uint8_t> bytes(NetProtocol::c_HeaderBytes, 0);
				bytes[0] = 0x43;
				bytes[1] = 0x43;
				bytes[2] = 0x4E;
				bytes[3] = 0x32; // "CCN2", the magic every version of this header starts with
				bytes[4] = static_cast<uint8_t>(version & 0xFFU);
				bytes[5] = static_cast<uint8_t>((version >> 8) & 0xFFU);
				bytes[6] = static_cast<uint8_t>(NetProtocol::c_HeaderBytes & 0xFFU);
				bytes[7] = static_cast<uint8_t>((NetProtocol::c_HeaderBytes >> 8) & 0xFFU);
				bytes[8] = static_cast<uint8_t>(messageType & 0xFFU);
				bytes[9] = static_cast<uint8_t>((messageType >> 8) & 0xFFU);
				return bytes;
			};

			// The transport hands a client the host's peer id in its own connect event.
			const auto hostPeerOf = [](LoopbackTransport& transport) {
				NetPeerId found = c_InvalidNetPeerId;
				for (const NetTransportEvent& event: transport.PollEvents()) {
					if (event.type == NetTransportEventType::PeerConnected) {
						found = event.peerId;
					}
				}
				return found;
			};

			// Arm A - the envelope allows it: an unknown message type under the SHARED header version.
			// This is the P6 case, a new client's message reaching a host that does not know the type.
			if (!oldWireTransport.Connect("loopback", port, &error)) {
				return Fail("the old-wire peer could not connect: " + error);
			}
			drive(nowMs + 100);
			{
				const NetPeerId hostPeer = hostPeerOf(oldWireTransport);
				if (hostPeer == c_InvalidNetPeerId) {
					return Fail("the old-wire peer never saw the host connect");
				}
				if (!oldWireTransport.Send(hostPeer, NetTransportLane::ControlReliable, envelope(NetProtocol::c_Version, 0x00F0), &error)) {
					return Fail("the old-wire peer could not send its unknown-type message: " + error);
				}
				drive(nowMs + 300);
				bool sawDecodableRejection = false;
				for (const NetTransportEvent& event: oldWireTransport.PollEvents()) {
					if (event.type != NetTransportEventType::PacketReceived) {
						continue;
					}
					const NetDecodeResult decoded = NetProtocol::Decode(event.bytes);
					if (decoded.ok && std::holds_alternative<NetJoinRejected>(decoded.message.payload)) {
						sawDecodableRejection = true;
					}
				}
				if (!sawDecodableRejection) {
					return Fail("an unknown message type under the shared header version got no decodable rejection");
				}
			}

			// Arm B - the envelope does not allow it: a header version whose payload schema this build
			// cannot write. The honest answer is no protocol message at all.
			if (!futureWireTransport.Connect("loopback", port, &error)) {
				return Fail("the future-wire peer could not connect: " + error);
			}
			drive(nowMs + 100);
			const uint32_t rejectionsBefore = host.GetStats().oldWireRejectionsSent;
			{
				const NetPeerId hostPeer = hostPeerOf(futureWireTransport);
				if (hostPeer == c_InvalidNetPeerId) {
					return Fail("the future-wire peer never saw the host connect");
				}
				const std::vector<uint8_t> future = envelope(static_cast<uint16_t>(NetProtocol::c_Version + 1), static_cast<uint16_t>(NetMessageType::ClientHello));
				if (!futureWireTransport.Send(hostPeer, NetTransportLane::ControlReliable, future, &error)) {
					return Fail("the future-wire peer could not send its hello: " + error);
				}
				drive(nowMs + 300);
				if (host.GetStats().oldWireDisconnects != 1) {
					return Fail("the unspeakable version was not counted as a best-effort disconnect");
				}
				if (host.GetStats().oldWireRejectionsSent != rejectionsBefore) {
					return Fail("the host sent a rejection stamped at a version the peer cannot decode");
				}
				bool disconnected = false;
				for (const NetTransportEvent& event: futureWireTransport.PollEvents()) {
					if (event.type == NetTransportEventType::PeerDisconnected) {
						disconnected = true;
						if (event.reason.find("protocol version") == std::string::npos) {
							return Fail("the best-effort disconnect carried no version reason");
						}
					}
					if (event.type == NetTransportEventType::PacketReceived) {
						return Fail("the host answered an unspeakable version with a protocol message");
					}
				}
				if (!disconnected) {
					return Fail("the unspeakable-version peer was not disconnected");
				}
			}

			// Neither arm may touch the live pair.
			if (!host.IsReady() || host.IsFailed() || host.GetReadyPeerCount() != readyBefore || !client.IsReady()) {
				return Fail("an old-wire peer disturbed the live session");
			}
			return 0;
		}

		// A2 wired the plane; this is the shape of the LIVE handshake it produces: with a plane
		// attached, Ready waits for JoinCommitted (§4), and the peer id the client ends up on is the
		// seat's own (P4), not the one AllocatePeerId handed out.
		int TestAdmissionGatesReady() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint16_t port = 42131;
			LoopbackTransport hostTransport;
			LoopbackTransport clientTransport;
			NetSession host;
			NetSession client;
			NetSeatAuthRegistry registry;
			registry.BeginHostedSession();
			NetReconnectHost admission;
			admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
			admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			NetReconnectTicketStore store;
			store.SetPath(StorePath("gate"));
			NetReconnectClient reconnect;
			uint64_t unixNow = 1700000000000ULL;
			reconnect.Configure(&store, MakeIdentity(), "Player");
			reconnect.SetUnixClock(&FixedUnixClock, &unixNow);
			reconnect.SetHostContext("loopback", MakeHash(5));
			host.SetReconnectHost(&admission);
			client.SetReconnectClient(&reconnect);
			if (!host.StartHost(hostTransport, MakeSessionConfig(port, 101, "Host"), &error) ||
			    !client.StartClient(clientTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not start the gated pair: " + error);
			}
			uint64_t nowMs = 0;
			bool sawGatedAccept = false;
			for (; nowMs <= 600; nowMs += 10) {
				host.Tick(nowMs);
				client.Tick(nowMs);
				// The window §4 opens: accepted by the session, not yet Ready, transaction in flight.
				if (client.GetState() == NetSessionState::Accepted && reconnect.IsAdmissionPending() && host.GetReadyPeerCount() == 0) {
					sawGatedAccept = true;
				}
				hostTransport.AdvanceTimeMs(10);
				clientTransport.AdvanceTimeMs(10);
			}
			if (!sawGatedAccept) {
				return Fail("the client declared itself Ready without waiting for a JoinCommitted");
			}
			if (!client.IsReady() || !host.IsReady() || reconnect.GetState() != NetH4ClientState::Joined) {
				return Fail("the gated handshake did not settle into a committed, ready session");
			}
			const std::vector<NetH4Seat> seats = MakeSeatTable();
			if (client.GetLocalPeerId() != seats[0].peerId) {
				return Fail("the client kept its allocated peer id instead of the seat's own");
			}
			const std::vector<NetSessionPeerInfo> readyPeers = host.GetReadyPeers();
			if (readyPeers.size() != 1 || readyPeers.front().assignedPeerId != seats[0].peerId) {
				return Fail("the host did not put the committed seat on its own peer id");
			}
			if (reconnect.UsedStoredTicket()) {
				return Fail("a first join claimed to have used a stored ticket");
			}

			// The negative control: with NO plane attached the handshake is exactly what it always was.
			{
				const uint16_t plainPort = 42132;
				LoopbackTransport plainHostTransport;
				LoopbackTransport plainClientTransport;
				NetSession plainHost;
				NetSession plainClient;
				if (!plainHost.StartHost(plainHostTransport, MakeSessionConfig(plainPort, 103, "Host"), &error) ||
				    !plainClient.StartClient(plainClientTransport, "loopback", MakeSessionConfig(plainPort, 204, "Player"), &error)) {
					return Fail("could not start the unwired pair: " + error);
				}
				for (uint64_t plainNow = 0; plainNow <= 300; plainNow += 10) {
					plainHost.Tick(plainNow);
					plainClient.Tick(plainNow);
					plainHostTransport.AdvanceTimeMs(10);
					plainClientTransport.AdvanceTimeMs(10);
				}
				if (!plainHost.IsReady() || !plainClient.IsReady() || plainClient.GetLocalPeerId() == 0) {
					return Fail("the session without an admission plane no longer reaches Ready on its own");
				}
			}
			return 0;
		}

	} // namespace

	int NetReconnectSessionSelfTest::Run() {
		if (const int result = TestStoreFailsClosed(); result != 0) {
			return result;
		}
		if (const int result = TestTicketStore(); result != 0) {
			return result;
		}
		if (const int result = TestTicketArtifactCanary(); result != 0) {
			return result;
		}
		if (const int result = TestFirstJoinTransaction(); result != 0) {
			return result;
		}
		if (const int result = TestProvisionalFailureWindows(); result != 0) {
			return result;
		}
		if (const int result = TestReclaimAndDuplicateProof(); result != 0) {
			return result;
		}
		if (const int result = TestNoEnumerationOracle(); result != 0) {
			return result;
		}
		if (const int result = TestChallengeAbuse(); result != 0) {
			return result;
		}
		if (const int result = TestSingleActiveIncarnation(); result != 0) {
			return result;
		}
		if (const int result = TestCleanLeaveAndAmbiguity(); result != 0) {
			return result;
		}
		if (const int result = TestLedgerAndReseat(); result != 0) {
			return result;
		}
		if (const int result = TestRejoinAcrossRematch(); result != 0) {
			return result;
		}
		if (const int result = TestSessionWiring(); result != 0) {
			return result;
		}
		if (const int result = TestSessionEndIsTheOnlySignal(); result != 0) {
			return result;
		}
		if (const int result = TestLobbyAdmissionIsolation(); result != 0) {
			return result;
		}
		if (const int result = TestSeatTableFromMatchConfig(); result != 0) {
			return result;
		}
		if (const int result = TestOldWireProbe(); result != 0) {
			return result;
		}
		if (const int result = TestAdmissionGatesReady(); result != 0) {
			return result;
		}
		std::cout << "[net-reconnect-session-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
