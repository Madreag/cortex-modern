#include "NetReconnectSessionSelfTest.h"

#include "ControllerFrame.h"
#include "LoopbackTransport.h"
#include "NetAuthCrypto.h"
#include "NetLobbySession.h"
#include "NetLockstep.h"
#include "NetMatchConfig.h"
#include "NetMatchRunner.h"
#include "NetReconnectLedger.h"
#include "NetReconnectSession.h"
#include "NetReconnectTicketStore.h"
#include "NetReconnectTranscript.h"
#include "NetReconnectUx.h"
#include "NetSeatAuth.h"
#include "NetSession.h"
#include "System/ScenarioRunner.h"
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

		// Brings one client to a committed seat and then drops its link MID-MATCH, which is the state
		// every reclaim test starts from - a lobby drop hands the seat back instead (A5).
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
			wire.host.SetLiveMatch(true);
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

			// The acknowledged leave clears the ticket and closes the seat. Mid-match: in a lobby the
			// seat goes back in the pool instead, which TestLobbySeatIsFreedForTheNextJoiner pins.
			wire.host.SetLiveMatch(true);
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

		// One settled actor, as the census sees it. The world the real census walks is not available
		// here; the resolver line below is production's, which is the part that was wrong.
		struct CensusActor {
			int64_t uid = 0;
			int32_t team = 0;
			bool cpu = false;
		};

		std::vector<CensusActor> g_ProductionCensusActors;

		// MovableMan::BuildLockstepOwnershipCensus resolves each actor's owner with exactly this call.
		uint8_t ProductionCensusOwner(const CensusActor& actor) {
			return ScenarioRunner::GetLockstepDropTimeActorOwner(actor.uid, actor.team, actor.cpu);
		}

		std::vector<NetH4LedgerActor> ProductionCensusSource(void*) {
			std::vector<NetH4LedgerActor> census;
			census.reserve(g_ProductionCensusActors.size());
			for (const CensusActor& actor: g_ProductionCensusActors) {
				census.push_back({actor.uid, actor.team, ProductionCensusOwner(actor), true});
			}
			return census;
		}

		// A three-peer round over loopback, driven far enough that the host adjudicates peer 2's drop -
		// which is the state the ledger's census is taken in.
		struct DroppedRound {
			LoopbackTransport hostT, aT, bT;
			NetLockstepCoordinator host, clientA, clientB;
			uint64_t now = 0;

			bool RunUntilPeerTwoIsGone(uint16_t port, const std::vector<NetMatchPlayerSlot>& players, std::string* error) {
				if (!hostT.StartHost(port, error) || !aT.Connect("loopback", port, error) || !bT.Connect("loopback", port, error)) {
					return false;
				}
				auto cfg = [&](uint8_t local, std::map<uint8_t, NetPeerId> transports, bool relay) {
					NetLockstepConfig c;
					c.sessionId = 0x7000000000000090ULL + port;
					c.timeoutMs = 5000;
					c.localPeerId = local;
					c.peerCount = 3;
					c.remoteTransportPeerIds = std::move(transports);
					c.relayToOtherPeers = relay;
					c.scenario = "ReconnectSessionSelfTest";
					c.matchConfig.hostPeerId = 1;
					c.matchConfig.peerCount = 3;
					c.matchConfig.players = players;
					return c;
				};
				if (!host.Start(hostT, cfg(1, {{2, 1}, {3, 2}}, true), error) ||
				    !clientA.Start(aT, cfg(2, {{1, 1}}, false), error) ||
				    !clientB.Start(bT, cfg(3, {{1, 1}}, false), error)) {
					return false;
				}
				auto drive = [&](uint64_t forMs, const std::function<bool()>& done) {
					for (const uint64_t until = now + forMs; now <= until; now += 5) {
						host.Tick(now);
						clientA.Tick(now);
						clientB.Tick(now);
						if (done()) {
							return true;
						}
						hostT.AdvanceTimeMs(5);
						aT.AdvanceTimeMs(5);
						bT.AdvanceTimeMs(5);
					}
					return false;
				};
				if (!drive(4000, [&] { return host.IsRunning() && clientA.IsRunning() && clientB.IsRunning(); })) {
					*error = "the ledger round did not reach Running";
					return false;
				}
				ControllerFrame frame;
				frame.stateMask = 1;
				for (uint64_t f = 0; f < 2; ++f) {
					frame.actorUniqueID = 100;
					if (!host.QueueLocalInput(f, {frame}, {}, error)) {
						return false;
					}
					frame.actorUniqueID = 200;
					if (!clientA.QueueLocalInput(f, {frame}, {}, error)) {
						return false;
					}
					frame.actorUniqueID = 300;
					if (!clientB.QueueLocalInput(f, {frame}, {}, error)) {
						return false;
					}
				}
				NetLockstepReadyFrame ready;
				size_t committed = 0;
				if (!drive(4000, [&] {
						while (host.PopReadyFrame(ready)) {
							++committed;
						}
						return committed >= 2;
					})) {
					*error = "the ledger round never committed a frame";
					return false;
				}
				// Peer 2's socket goes away: the relay host adjudicates it as a leave, and from there on
				// ResolveActorOwner renames its units.
				aT.Stop();
				if (!drive(2000, [&] { return host.GetPeerLeaveFrames().count(2) != 0; })) {
					*error = "the drop was never adjudicated as a leave";
					return false;
				}
				return true;
			}
		};

		// The ledger records who HELD a seat's units at the drop. The round renames a leaver's units the
		// moment it adjudicates the leave - to a surviving teammate, or to the relay host while the seat
		// is held - so a census taken after that names anyone but the leaver, and a ledger filtered on the
		// leaver's id comes back empty. An empty record makes IssueReseat return before it issues, which
		// is a returner reseated onto nothing.
		int TestLedgerRecordsWhatTheLeaverHeld() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			struct Arm {
				const char* label;
				uint16_t port;
				std::vector<NetMatchPlayerSlot> players;
				uint8_t renamedOwner;    //!< Who the leave hands peer 2's units to.
				int64_t handedToPeerThree; //!< A unit peer 3 already holds, so it is not peer 2's to get back.
				std::vector<int64_t> expected;
			};
			const std::vector<Arm> arms = {
			    // Co-op shape: peers 2 and 3 share team 1, so the leave hands peer 2's units to peer 3 and
			    // they must hand back per the ledger.
			    {"survivor", 42180, {{1, 0, false, "Host"}, {2, 1, false, "A"}, {3, 1, false, "B"}}, 3, 103, {101, 102}},
			    // The 1v1 shape the H4 gates run: nobody is left on team 1, so A6's fallback answers.
			    {"no-survivor", 42184, {{1, 0, false, "Host"}, {2, 1, false, "A"}, {3, 2, false, "B"}}, 0, 0, {101, 102, 103}},
			};

			for (const Arm& arm: arms) {
				const std::string where = std::string(" (") + arm.label + " arm)";
				DroppedRound round;
				if (!round.RunUntilPeerTwoIsGone(arm.port, arm.players, &error)) {
					return Fail(error + where);
				}
				ScenarioRunner::SetLockstepCoordinator(&round.host);
				if (arm.handedToPeerThree != 0) {
					ScenarioRunner::SetLockstepControlOverride(arm.handedToPeerThree, 3);
				}
				g_ProductionCensusActors = {{101, 1, false}, {102, 1, false}, {103, 1, false}, {201, 2, false}};

				auto fail = [&](const std::string& message) {
					ScenarioRunner::SetLockstepCoordinator(nullptr);
					return Fail(message + where);
				};
				// The rename is the whole point: without it the census would still name the leaver.
				if (round.host.ResolveActorOwner(101, 1, false) != arm.renamedOwner) {
					return fail("the leave did not rename the dropped peer's units");
				}

				uint64_t unixNow = 1'700'000'000'000ULL;
				Wire wire;
				ConfigureWire(wire);
				wire.host.SetDropOwnershipSource(&ProductionCensusSource, nullptr);
				Endpoint player;
				player.connection = 121;
				ConfigureEndpoint(player, std::string("held-") + arm.label, &unixNow);
				wire.Add(&player);
				NetH4TicketRecord record;
				if (SeatAndDrop(wire, player, record, unixNow, &error) != 0) {
					return fail("could not seat the player: " + error);
				}
				const NetH4SeatOwnership* ledgered = wire.host.GetLedger().Find(0);
				if (ledgered == nullptr || ledgered->actorUIDs != arm.expected) {
					return fail("the drop ledger did not record the units the leaver held");
				}

				// And the reseat the returner is given is issued from it.
				wire.host.SetLiveMatch(true);
				Endpoint returner;
				returner.connection = 122;
				ConfigureEndpoint(returner, std::string("held-") + arm.label, &unixNow);
				wire.Add(&returner);
				wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
				if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
					return fail("the reclaim did not settle: " + error);
				}
				const std::vector<NetGameReseat> reseats = wire.host.TakePendingReseats();
				if (reseats.size() != 1 || reseats.front().newOwnerPeerId != MakeSeatTable()[0].lockstepPeerId ||
				    reseats.front().team != 1 || reseats.front().actorUIDs != arm.expected) {
					return fail("the reclaim did not issue the ledgered reseat");
				}
				if (wire.host.GetStats().reseatsIssued != 1) {
					return fail("the reseat was not counted");
				}
				ScenarioRunner::SetLockstepCoordinator(nullptr);
			}
			g_ProductionCensusActors.clear();
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

		// A2 limit 4: a reclaiming peer sits in the session's pre-Ready state until it commits, so the
		// whole ladder has to fit the host's timeout. Budget: each of the two steps (Reclaim->Challenge,
		// Proof->JoinCommitted) retransmits at P3's 250 ms up to 8 times = 2 000 ms, so the transaction
		// gives up at 4 000 ms - 1 000 ms inside the 5 000 ms production timeoutMs, and the client's own
		// retransmits keep refreshing the host's lastReceiveMs the whole way.
		int TestReclaimLadderFitsHandshakeTimeout() {
			static_assert(NetReconnectHost::c_RetransmitIntervalMs * NetReconnectHost::c_MaxRetransmits == 2000,
			              "P3's ladder is 250 ms x 8");
			static_assert(2 * NetReconnectHost::c_RetransmitIntervalMs * NetReconnectHost::c_MaxRetransmits < 5000,
			              "two reclaim steps must fit inside the 5 000 ms session timeout");
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint16_t port = 42133;
			LoopbackTransport hostTransport;
			LoopbackTransport firstTransport;
			NetSession host;
			NetSession first;
			NetSeatAuthRegistry registry;
			registry.BeginHostedSession();
			NetReconnectHost admission;
			admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
			admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			NetReconnectTicketStore store;
			store.SetPath(StorePath("ladder"));
			NetReconnectClient firstClient;
			uint64_t unixNow = 1700000000000ULL;
			firstClient.Configure(&store, MakeIdentity(), "Player");
			firstClient.SetUnixClock(&FixedUnixClock, &unixNow);
			firstClient.SetHostContext("loopback", MakeHash(5));
			host.SetReconnectHost(&admission);
			first.SetReconnectClient(&firstClient);
			if (!host.StartHost(hostTransport, MakeSessionConfig(port, 101, "Host"), &error) ||
			    !first.StartClient(firstTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not seat the first holder: " + error);
			}
			uint64_t nowMs = 0;
			for (; nowMs <= 600; nowMs += 10) {
				host.Tick(nowMs);
				first.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				firstTransport.AdvanceTimeMs(10);
			}
			if (firstClient.GetState() != NetH4ClientState::Joined || !store.HasRecord()) {
				return Fail("the first holder never committed a seat to reclaim");
			}
			// The ladder this case measures is the MID-MATCH reclaim; the match has to be live before
			// the drop or the seat is handed back to the lobby pool instead (A5).
			admission.SetLiveMatch(true);
			firstTransport.Stop();
			for (const uint64_t settleUntil = nowMs + 200; nowMs <= settleUntil; nowMs += 10) {
				host.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
			}

			// The returner runs its whole ladder at 200 ms of latency - the top of the target band, so
			// every step really spends part of its budget rather than settling inside one tick.
			LoopbackTransport returnerTransport;
			LoopbackTransportConfig lag;
			lag.latencyMs = 200;
			returnerTransport.SetFaultConfig(lag);
			hostTransport.SetFaultConfig(lag);
			NetSession returner;
			NetReconnectClient returnerClient;
			returnerClient.Configure(&store, MakeIdentity(), "Player");
			returnerClient.SetUnixClock(&FixedUnixClock, &unixNow);
			returnerClient.SetHostContext("loopback", MakeHash(5));
			returner.SetReconnectClient(&returnerClient);
			if (!returner.StartClient(returnerTransport, "loopback", MakeSessionConfig(port, 303, "Player"), &error)) {
				return Fail("the returner could not connect: " + error);
			}
			const uint64_t reclaimStartMs = nowMs;
			uint64_t committedAtMs = 0;
			for (const uint64_t deadline = reclaimStartMs + 8000; nowMs <= deadline; nowMs += 10) {
				host.Tick(nowMs);
				returner.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				returnerTransport.AdvanceTimeMs(10);
				if (committedAtMs == 0 && returnerClient.GetState() == NetH4ClientState::Joined) {
					committedAtMs = nowMs;
				}
			}
			if (committedAtMs == 0) {
				return Fail("the reclaim never committed inside the ladder");
			}
			const uint64_t elapsedMs = committedAtMs - reclaimStartMs;
			if (elapsedMs > 2 * NetReconnectHost::c_RetransmitIntervalMs * NetReconnectHost::c_MaxRetransmits) {
				return Fail("the reclaim outran its two-step P3 budget: " + std::to_string(elapsedMs) + " ms");
			}
			if (elapsedMs >= MakeSessionConfig(port, 0, "Host").timeoutMs) {
				return Fail("the reclaim did not fit inside the session timeout");
			}
			if (host.GetStats().timeouts != 0) {
				return Fail("the host timed a peer out during the reclaim ladder");
			}
			if (!returner.IsReady() || returner.GetLocalPeerId() != MakeSeatTable()[0].peerId) {
				return Fail(std::string("the returner did not end up Ready on the seat's own peer id: state=") +
				            NetSession::StateName(returner.GetState()) + " peer=" + std::to_string(returner.GetLocalPeerId()) +
				            " expected=" + std::to_string(MakeSeatTable()[0].peerId) + " reject=" + returner.BuildRejectText() +
				            " client=" + NetReconnectClientStateName(returnerClient.GetState()));
			}
			if (admission.GetStats().reclaimsAccepted != 1) {
				return Fail("the host recorded no accepted reclaim");
			}
			if (!returnerClient.UsedStoredTicket()) {
				return Fail("the returner did not reclaim from the stored record");
			}
			// The seam this case exists for: P3 retransmits every 250 ms while P16 admits one attempt a
			// second, so a legitimate ladder must not be charged to the rate limit or answered with a
			// denial. The lag is what makes the ladder actually retransmit.
			if (admission.GetStats().reclaimRetransmitsDropped == 0 && returnerClient.GetStats().retransmits == 0) {
				return Fail("the ladder never retransmitted, so this did not exercise the P3/P16 seam");
			}
			if (admission.GetStats().denialsScheduled != 0) {
				return Fail("a legitimate retransmit was denied: " + std::to_string(admission.GetStats().denialsScheduled) + " denials");
			}
			return 0;
		}

		// §11's schedule, entirely on an injected clock: one attempt per full P3 ladder for the whole P2
		// resume window, cancel and manual retry, and the four startup-offer outcomes.
		int TestReconnectUxSchedule() {
			static_assert(NetReconnectUx::c_AttemptIntervalMs == 2000, "one attempt per P3 ladder");
			static_assert(NetReconnectUx::c_ResumeWindowMs == 20000, "the P2 resume window");
			static_assert(NetReconnectUx::c_MaxAttempts == 10, "ten attempts fit the window");

			NetReconnectUx ux;
			if (ux.GetState() != NetReconnectUxState::Idle || ux.IsActive() || !ux.GetStatusText().empty()) {
				return Fail("a fresh reconnect UX was not idle and silent");
			}
			uint64_t nowMs = 1000;
			ux.NoteConnected(nowMs);
			if (ux.Tick(nowMs)) {
				return Fail("a connected UX asked for a reconnect attempt");
			}
			ux.NoteDropped(nowMs, "connection closed by peer");
			if (!ux.IsActive() || ux.GetStatusText().find("Reconnecting") == std::string::npos ||
			    ux.GetStatusText().find("connection closed by peer") == std::string::npos) {
				return Fail("the drop did not produce a persistent reason-carrying status");
			}
			// The first attempt is due at once; each later one waits a whole ladder.
			std::vector<uint64_t> attemptsAt;
			for (uint32_t attempt = 0; attempt < NetReconnectUx::c_MaxAttempts; ++attempt) {
				bool fired = false;
				for (uint64_t step = 0; step < 400 && !fired; ++step, nowMs += 10) {
					if (ux.Tick(nowMs)) {
						fired = true;
						attemptsAt.push_back(nowMs);
						ux.NoteAttemptStarted(nowMs);
						ux.NoteAttemptFailed(nowMs, "host did not answer");
					}
				}
				if (!fired) {
					return Fail("attempt " + std::to_string(attempt + 1) + " never came due");
				}
				if (ux.GetAttempts() != attempt + 1) {
					return Fail("the attempt count did not follow the schedule");
				}
			}
			for (size_t attempt = 1; attempt < attemptsAt.size(); ++attempt) {
				const uint64_t gap = attemptsAt[attempt] - attemptsAt[attempt - 1];
				if (gap < NetReconnectUx::c_AttemptIntervalMs || gap > NetReconnectUx::c_AttemptIntervalMs + 10) {
					return Fail("attempts were not one P3 ladder apart: " + std::to_string(gap) + " ms");
				}
			}
			if (attemptsAt.back() - attemptsAt.front() >= NetReconnectUx::c_ResumeWindowMs) {
				return Fail("the schedule ran past the resume window it is budgeted against");
			}
			if (ux.GetState() != NetReconnectUxState::GaveUp || ux.Tick(nowMs)) {
				return Fail("the UX kept trying after its attempts were spent");
			}
			if (!ux.CanRetryManually() || ux.CanCancel()) {
				return Fail("a spent schedule did not offer exactly a manual retry");
			}
			ux.RequestManualRetry(nowMs);
			if (ux.GetState() != NetReconnectUxState::Waiting || ux.GetAttempts() != 0 || !ux.Tick(nowMs)) {
				return Fail("the manual retry did not reopen the window");
			}
			ux.NoteAttemptStarted(nowMs);
			if (!ux.CanCancel()) {
				return Fail("an in-flight attempt could not be cancelled");
			}
			ux.Cancel(nowMs);
			if (ux.GetState() != NetReconnectUxState::Cancelled || ux.Tick(nowMs + 100000)) {
				return Fail("a cancelled schedule kept firing");
			}
			if (!ux.CanRetryManually()) {
				return Fail("a cancel removed the manual retry as well");
			}
			ux.RequestManualRetry(nowMs);
			ux.NoteAttemptStarted(nowMs);
			ux.NoteReconnected(nowMs);
			if (ux.GetState() != NetReconnectUxState::Reconnected || ux.IsActive() || ux.GetStatusText() != "Reconnected.") {
				return Fail("a successful reconnect did not settle the banner");
			}

			// The window closing is a real end, not just a spent counter.
			{
				NetReconnectUx expired;
				expired.NoteDropped(5000, "link lost");
				expired.NoteAttemptStarted(5000);
				expired.NoteAttemptFailed(5000 + NetReconnectUx::c_ResumeWindowMs + 1, "");
				if (expired.GetState() != NetReconnectUxState::GaveUp) {
					return Fail("an attempt that failed past the resume window kept the schedule alive");
				}
			}

			// The startup offer distinguishes what the store found; §11 requires exactly that.
			{
				NetReconnectUx offer;
				offer.OfferStoredTicket(NetH4TicketLoadResult::Loaded, "10.0.0.7");
				if (offer.GetOffer() != NetReconnectOffer::Available || offer.GetOfferAddress() != "10.0.0.7" ||
				    offer.GetOfferText().find("10.0.0.7") == std::string::npos) {
					return Fail("a usable record was not offered with its host");
				}
				offer.OfferStoredTicket(NetH4TicketLoadResult::Corrupt, "10.0.0.7");
				if (offer.GetOffer() != NetReconnectOffer::Corrupt || !offer.GetOfferAddress().empty() ||
				    offer.GetOfferText().find("damaged") == std::string::npos) {
					return Fail("a damaged record was not reported as damaged");
				}
				offer.OfferStoredTicket(NetH4TicketLoadResult::Stale, "10.0.0.7");
				if (offer.GetOffer() != NetReconnectOffer::Stale || offer.GetOfferText().find("too old") == std::string::npos) {
					return Fail("a stale record was not reported as stale");
				}
				offer.OfferStoredTicket(NetH4TicketLoadResult::Missing, "");
				if (offer.GetOffer() != NetReconnectOffer::None || !offer.GetOfferText().empty()) {
					return Fail("a missing record produced an offer");
				}
				offer.OfferStoredTicket(NetH4TicketLoadResult::Loaded, "10.0.0.7");
				offer.DismissOffer();
				if (offer.GetOffer() != NetReconnectOffer::None) {
					return Fail("the offer could not be dismissed");
				}
			}

			// The roster mark is persistent text, not a toast, and reclaiming outranks dropped.
			if (std::string(NetReconnectUx::RosterMark(false, false)) != "" ||
			    std::string(NetReconnectUx::RosterMark(true, false)).find("Disconnected") == std::string::npos ||
			    std::string(NetReconnectUx::RosterMark(true, true)).find("Reconnecting") == std::string::npos) {
				return Fail("the roster mark did not describe the seat");
			}
			return 0;
		}

		// §4's other half: a seat whose holder DROPPED stays worth waiting for until the P2 window
		// closes, so a round with nobody left does not end under a player who is coming back. The window
		// governs the round, not the ticket - the seat stays reclaimable either way.
		int TestSeatHoldWindow() {
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
			player.connection = 91;
			ConfigureEndpoint(player, "hold", &unixNow);
			wire.Add(&player);
			if (!player.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the seeding join did not settle: " + error);
			}
			NetH4TicketRecord record;
			if (player.store.Load(unixNow, record, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail(error);
			}
			const std::vector<NetH4Seat> seats = MakeSeatTable();
			const uint8_t held = seats[0].lockstepPeerId;
			if (!wire.host.IsSeatHeldForReclaim(held)) {
				return Fail("a committed seat did not read as worth waiting for");
			}
			if (wire.host.IsSeatHeldForReclaim(seats[1].lockstepPeerId)) {
				return Fail("a seat nobody has ever held read as worth waiting for");
			}
			wire.host.SetLiveMatch(true);

			// The drop, then the whole P2 window: held throughout, and not one millisecond past it.
			wire.host.NotifyDisconnect(player.connection, 120);
			player.connected = false;
			if (!wire.host.IsSeatHeldForReclaim(held)) {
				return Fail("a dropped holder stopped being worth waiting for the moment it dropped");
			}
			wire.host.Tick(wire.nowMs + NetReconnectHost::c_ProvisionalExpiryMs);
			if (!wire.host.IsSeatHeldForReclaim(held)) {
				return Fail("the hold ended inside the P2 window");
			}
			wire.nowMs += NetReconnectHost::c_ProvisionalExpiryMs + 1;
			wire.host.Tick(wire.nowMs);
			if (wire.host.IsSeatHeldForReclaim(held)) {
				return Fail("the hold outlived the P2 window");
			}
			if (wire.host.GetStats().seatHoldsExpired != 1) {
				return Fail("the closed window was not counted");
			}
			wire.host.Tick(wire.nowMs + 5000);
			if (wire.host.GetStats().seatHoldsExpired != 1) {
				return Fail("the closed window was counted more than once");
			}

			// The narrowness this case exists for: a closed window ends the ROUND's wait, not the seat's
			// ticket. The same holder still reclaims, and the seat is worth waiting for again.
			Endpoint returner;
			returner.connection = 92;
			ConfigureEndpoint(returner, "hold-return", &unixNow);
			wire.Add(&returner);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the reclaim after the window closed did not settle: " + error);
			}
			if (returner.client.GetState() != NetH4ClientState::Joined) {
				return Fail("a closed hold window refused the seat's own ticket");
			}
			if (!wire.host.IsSeatHeldForReclaim(held)) {
				return Fail("a reclaimed seat did not become worth waiting for again");
			}

			// A clean leave closes the seat, so a round with nobody left ends at once rather than waiting.
			if (!returner.client.BeginLeave(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the leave did not settle: " + error);
			}
			if (wire.host.GetStats().seatsClosedByLeave != 1) {
				return Fail("the leave did not close the seat");
			}
			if (wire.host.IsSeatHeldForReclaim(held)) {
				return Fail("a seat closed by a clean leave is still being waited for");
			}
			return 0;
		}

		// §7's ordering: the leave has to be answered while the link is still up. The negative control
		// is the defect this replaces - tearing the link down first leaves the ticket unanswered.
		int TestLeaveExchangeBeatsTeardown() {
			static_assert(NetReconnectClient::c_LeaveAckBudgetMs == 2000, "P21's ack budget");
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			auto seat = [&](const char* name, uint16_t port, uint64_t nonce, LoopbackTransport& hostTransport,
			                LoopbackTransport& clientTransport, NetSession& host, NetSession& client,
			                NetSeatAuthRegistry& registry, NetReconnectHost& admission, NetReconnectTicketStore& store,
			                NetReconnectClient& reconnect, uint64_t* unixNow, uint64_t& nowMs) {
				registry.BeginHostedSession();
				admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
				admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
				store.SetPath(StorePath(name));
				reconnect.Configure(&store, MakeIdentity(), "Player");
				reconnect.SetUnixClock(&FixedUnixClock, unixNow);
				reconnect.SetHostContext("loopback", MakeHash(5));
				host.SetReconnectHost(&admission);
				client.SetReconnectClient(&reconnect);
				if (!host.StartHost(hostTransport, MakeSessionConfig(port, nonce, "Host"), &error) ||
				    !client.StartClient(clientTransport, "loopback", MakeSessionConfig(port, nonce + 1, "Player"), &error)) {
					return false;
				}
				for (nowMs = 0; nowMs <= 600; nowMs += 10) {
					host.Tick(nowMs);
					client.Tick(nowMs);
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
				}
				return reconnect.GetState() == NetH4ClientState::Joined && store.HasRecord();
			};

			uint64_t unixNow = 1'700'000'000'000ULL;
			// The leave the engine now runs: the link is still up, so the ack arrives and clears the record.
			{
				LoopbackTransport hostTransport, clientTransport;
				NetSession host, client;
				NetSeatAuthRegistry registry;
				NetReconnectHost admission;
				NetReconnectTicketStore store;
				NetReconnectClient reconnect;
				uint64_t nowMs = 0;
				if (!seat("leave-live", 42135, 111, hostTransport, clientTransport, host, client, registry, admission, store, reconnect, &unixNow, nowMs)) {
					return Fail("the leaving holder never committed a seat: " + error);
				}
				if (!reconnect.BeginLeave(client.GetClockMs(), &error)) {
					return Fail("the leave would not start: " + error);
				}
				const uint64_t startedMs = nowMs;
				uint64_t acknowledgedMs = 0;
				for (const uint64_t until = nowMs + NetReconnectClient::c_LeaveAckBudgetMs; nowMs <= until; nowMs += 10) {
					host.Tick(nowMs);
					client.Tick(nowMs);
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
					if (acknowledgedMs == 0 && reconnect.GetState() == NetH4ClientState::Left) {
						acknowledgedMs = nowMs;
					}
				}
				if (acknowledgedMs == 0) {
					return Fail("the leave was never acknowledged on a live link");
				}
				if (acknowledgedMs - startedMs > NetReconnectClient::c_LeaveAckBudgetMs) {
					return Fail("the leave outran P21's ack budget");
				}
				if (store.HasRecord() || reconnect.GetStats().leaveAcksReceived != 1 || reconnect.GetStats().ticketsCleared != 1) {
					return Fail("an acknowledged leave did not clear the recovery record");
				}
				if (admission.GetStats().seatsClosedByLeave != 1) {
					return Fail("the host did not close the seat on the leave");
				}
			}

			// The negative control: the link goes down FIRST, which is what running the exchange from
			// teardown amounts to. Nothing is acknowledged and §7 keeps the record.
			{
				LoopbackTransport hostTransport, clientTransport;
				NetSession host, client;
				NetSeatAuthRegistry registry;
				NetReconnectHost admission;
				NetReconnectTicketStore store;
				NetReconnectClient reconnect;
				uint64_t nowMs = 0;
				if (!seat("leave-late", 42136, 121, hostTransport, clientTransport, host, client, registry, admission, store, reconnect, &unixNow, nowMs)) {
					return Fail("the late-leaving holder never committed a seat: " + error);
				}
				clientTransport.Stop();
				reconnect.NotifyAmbiguousLoss();
				(void)reconnect.BeginLeave(client.GetClockMs(), &error);
				for (const uint64_t until = nowMs + NetReconnectClient::c_LeaveAckBudgetMs + 500; nowMs <= until; nowMs += 10) {
					host.Tick(nowMs);
					client.Tick(nowMs);
					hostTransport.AdvanceTimeMs(10);
					clientTransport.AdvanceTimeMs(10);
				}
				if (reconnect.GetState() == NetH4ClientState::Left) {
					return Fail("a leave sent after the link went down was somehow acknowledged");
				}
				if (!store.HasRecord() || reconnect.GetStats().leaveAcksReceived != 0) {
					return Fail("an unacknowledged leave deleted the recovery record");
				}
				if (admission.GetStats().seatsClosedByLeave != 0) {
					return Fail("the host closed a seat nobody asked it to close");
				}
			}
			return 0;
		}

		// §6 over two real sessions, on a peer-id space the first incarnation has saturated: the seat's
		// own id is taken, so the returner proves on a provisional one and the commit hands it the
		// seat's. Without this the host answers SessionFull and 1v1 fencing cannot happen at all.
		int TestReturningHolderOnAFullSession() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint16_t port = 42134;
			LoopbackTransport hostTransport;
			LoopbackTransport firstTransport;
			NetSession host;
			NetSession first;
			NetSeatAuthRegistry registry;
			registry.BeginHostedSession();
			NetReconnectHost admission;
			admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
			admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			NetReconnectTicketStore store;
			store.SetPath(StorePath("fence-live"));
			NetReconnectClient firstClient;
			uint64_t unixNow = 1'700'000'000'000ULL;
			firstClient.Configure(&store, MakeIdentity(), "Player");
			firstClient.SetUnixClock(&FixedUnixClock, &unixNow);
			firstClient.SetHostContext("loopback", MakeHash(5));
			host.SetReconnectHost(&admission);
			first.SetReconnectClient(&firstClient);
			// The 1v1 shape: exactly one remote peer id exists, and the first holder is on it.
			NetSessionConfig hostConfig = MakeSessionConfig(port, 101, "Host");
			hostConfig.maxPeers = 1;
			if (!host.StartHost(hostTransport, hostConfig, &error) ||
			    !first.StartClient(firstTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not seat the first holder: " + error);
			}
			uint64_t nowMs = 0;
			for (; nowMs <= 600; nowMs += 10) {
				host.Tick(nowMs);
				first.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				firstTransport.AdvanceTimeMs(10);
			}
			if (firstClient.GetState() != NetH4ClientState::Joined || !store.HasRecord() || host.GetReadyPeerCount() != 1) {
				return Fail("the first holder never committed the only seat");
			}

			// The negative control first: outside a live match a full session is simply full.
			{
				LoopbackTransport lobbyTransport;
				NetSession lobbyJoiner;
				NetReconnectTicketStore lobbyStore;
				lobbyStore.SetPath(StorePath("fence-live-lobby"));
				NetReconnectClient lobbyClient;
				lobbyClient.Configure(&lobbyStore, MakeIdentity(), "Player");
				lobbyClient.SetUnixClock(&FixedUnixClock, &unixNow);
				lobbyClient.SetHostContext("loopback", MakeHash(5));
				lobbyJoiner.SetReconnectClient(&lobbyClient);
				if (!lobbyJoiner.StartClient(lobbyTransport, "loopback", MakeSessionConfig(port, 404, "Player"), &error)) {
					return Fail("the lobby control could not connect: " + error);
				}
				for (const uint64_t until = nowMs + 600; nowMs <= until; nowMs += 10) {
					host.Tick(nowMs);
					first.Tick(nowMs);
					lobbyJoiner.Tick(nowMs);
					hostTransport.AdvanceTimeMs(10);
					firstTransport.AdvanceTimeMs(10);
					lobbyTransport.AdvanceTimeMs(10);
				}
				if (lobbyJoiner.GetRejectReason() != NetRejectReason::SessionFull) {
					return Fail(std::string("a full lobby admitted a third connection: ") + lobbyJoiner.BuildRejectText());
				}
				if (host.GetStats().pendingAdmissionJoins != 0) {
					return Fail("a lobby joiner was given a provisional admission id");
				}
			}

			// Live match, and the second incarnation arrives while the first is STILL connected.
			admission.SetLiveMatch(true);
			LoopbackTransport secondTransport;
			NetSession second;
			NetReconnectClient secondClient;
			secondClient.Configure(&store, MakeIdentity(), "Player");
			secondClient.SetUnixClock(&FixedUnixClock, &unixNow);
			secondClient.SetHostContext("loopback", MakeHash(5));
			second.SetReconnectClient(&secondClient);
			if (!second.StartClient(secondTransport, "loopback", MakeSessionConfig(port, 303, "Player"), &error)) {
				return Fail("the second incarnation could not connect: " + error);
			}
			for (const uint64_t until = nowMs + 4000; nowMs <= until; nowMs += 10) {
				host.Tick(nowMs);
				first.Tick(nowMs);
				second.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				firstTransport.AdvanceTimeMs(10);
				secondTransport.AdvanceTimeMs(10);
			}
			if (secondClient.GetState() != NetH4ClientState::Joined) {
				return Fail(std::string("the second incarnation did not commit: ") + NetReconnectClientStateName(secondClient.GetState()) +
				            " session=" + NetSession::StateName(second.GetState()) + " reject=" + second.BuildRejectText());
			}
			if (!secondClient.UsedStoredTicket() || secondClient.GetIncarnation() != 2) {
				return Fail("the second incarnation did not supersede the first");
			}
			if (host.GetStats().pendingAdmissionJoins != 1) {
				return Fail("the returner was not admitted on a provisional id");
			}
			if (admission.GetStats().incarnationsBound != 2 || admission.GetStats().reclaimsAccepted != 1) {
				return Fail("the host did not bind a second incarnation of the seat");
			}
			if (!second.IsReady() || second.GetLocalPeerId() != MakeSeatTable()[0].peerId) {
				return Fail("the returner did not end up Ready on the seat's own peer id");
			}
			if (host.GetStats().fencedDisconnects + host.GetStats().fencedPackets == 0) {
				return Fail("the superseded transport was never counted as fenced");
			}
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!admission.GetSeatHolder(0, holder, generation, incarnation) || incarnation != 2) {
				return Fail("the seat did not move to the newer incarnation");
			}
			if (host.GetReadyPeerCount() != 1) {
				return Fail("the host ended up with something other than one live holder");
			}
			return 0;
		}

		// A5: the H4 admission rules are a MATCH feature. In a lobby nothing has been played, so a
		// member who leaves or drops has nothing to reclaim and its seat must go back in the pool -
		// otherwise a replacement is refused SessionFull and the lobby can never be refilled.
		int TestLobbySeatIsFreedForTheNextJoiner() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			const std::vector<NetH4Seat> seats = MakeSeatTable();
			Endpoint departing;
			departing.connection = 101;
			ConfigureEndpoint(departing, "lobby-departing", &unixNow);
			wire.Add(&departing);
			if (!departing.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the lobby join did not settle: " + error);
			}
			if (departing.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the first lobby member never joined");
			}
			NetPeerId holder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(seats[0].stableSeat, holder, generation, incarnation)) {
				return Fail("the lobby join took no seat");
			}

			// The clean leave: the seat comes back, it is not closed against the next joiner.
			if (!departing.client.BeginLeave(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the lobby leave did not settle: " + error);
			}
			if (departing.client.GetState() != NetH4ClientState::Left) {
				return Fail("the lobby leave was not acknowledged");
			}
			if (wire.host.GetStats().seatsReleasedInLobby != 1) {
				return Fail("the lobby leave did not hand the seat back");
			}
			if (wire.host.IsSeatClosed(seats[0].stableSeat) || wire.host.IsSeatHeldForReclaim(seats[0].lockstepPeerId)) {
				return Fail("a lobby seat was closed or held after its member left");
			}
			departing.connected = false;

			// The replacement is a FRESH runtime with no ticket - exactly what the lobby lanes launch.
			Endpoint replacement;
			replacement.connection = 102;
			ConfigureEndpoint(replacement, "lobby-replacement", &unixNow);
			wire.Add(&replacement);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!replacement.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the replacement's join did not settle: " + error);
			}
			if (replacement.client.GetState() != NetH4ClientState::Joined) {
				return Fail(std::string("the replacement was refused the freed lobby seat: ") +
				            NetReconnectClientStateName(replacement.client.GetState()));
			}
			if (wire.host.GetStats().provisionalSeatsRefused != 0) {
				return Fail("a lobby joiner was refused while a seat was free");
			}

			// A lobby DROP frees the seat the same way - the lanes' --action drop arm.
			if (wire.host.NotifyDisconnect(replacement.connection, 0) != NetH4DisconnectOutcome::SeatDropped) {
				return Fail("the lobby drop was not seen as the seat's holder going away");
			}
			replacement.connected = false;
			if (wire.host.GetStats().seatsReleasedInLobby != 2 || wire.host.GetStats().seatsDropped != 0) {
				return Fail("a lobby drop was recorded as a mid-match seat drop");
			}
			Endpoint second;
			second.connection = 103;
			ConfigureEndpoint(second, "lobby-second", &unixNow);
			wire.Add(&second);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!second.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error) ||
			    second.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the seat a lobby drop freed was not joinable");
			}

			// The control: in a LIVE match none of this changes. A drop still records the ownership and
			// holds the seat, and it is not handed to anybody else.
			wire.host.SetLiveMatch(true);
			const uint32_t releasedBefore = wire.host.GetStats().seatsReleasedInLobby;
			if (wire.host.NotifyDisconnect(second.connection, 240) != NetH4DisconnectOutcome::SeatDropped) {
				return Fail("the mid-match drop was not a seat drop");
			}
			second.connected = false;
			if (wire.host.GetStats().seatsDropped != 1 || wire.host.GetStats().seatsReleasedInLobby != releasedBefore) {
				return Fail("a mid-match drop released the seat instead of holding it");
			}
			if (!wire.host.IsSeatHeldForReclaim(seats[0].lockstepPeerId)) {
				return Fail("a mid-match drop stopped holding the seat for its reclaim window");
			}
			Endpoint intruder;
			intruder.connection = 104;
			ConfigureEndpoint(intruder, "lobby-intruder", &unixNow);
			wire.Add(&intruder);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!intruder.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the mid-match ticketless join did not settle: " + error);
			}
			if (intruder.client.GetState() == NetH4ClientState::Joined) {
				return Fail("a ticketless joiner took a held seat out of a live match");
			}
			return 0;
		}

		// A5: §11's recovery is a MATCH feature. A lobby that never started has no seat to reclaim, and
		// a retry there drags the player back into a lobby that is gone instead of to the menu.
		int TestRecoveryAppliesOnlyAfterAMatch() {
			static_assert(NetReconnectUx::RecoveryApplies(true, false, true, true), "a mid-match loss recovers");
			static_assert(!NetReconnectUx::RecoveryApplies(true, false, true, false), "a lobby loss does not");
			static_assert(!NetReconnectUx::RecoveryApplies(true, true, true, true), "the host does not recover its own session");
			static_assert(!NetReconnectUx::RecoveryApplies(true, false, false, true), "no record, nothing to prove");
			static_assert(!NetReconnectUx::RecoveryApplies(false, false, true, true), "a session that did not fail is not recovering");
			// The schedule itself is unchanged, and still runs once the rule admits the loss.
			NetReconnectUx ux;
			ux.NoteConnected(0);
			ux.NoteDropped(1000, "connection lost");
			if (!ux.IsActive() || !ux.Tick(1000)) {
				return Fail("a mid-match drop no longer starts the recovery schedule");
			}
			ux.NoteAttemptStarted(1000);
			ux.NoteReconnected(1200);
			if (ux.GetState() != NetReconnectUxState::Reconnected) {
				return Fail("the schedule did not settle back into Reconnected");
			}
			return 0;
		}


		// Builds an Applicant the way a client would, so the raw-connection cases ride the same bytes.
		NetH4Applicant MakeApplicant(uint16_t stableSeat, uint8_t txSeed, const std::string& name) {
			NetH4Applicant applicant;
			applicant.txId = Ramp<16>(txSeed);
			applicant.stableSeat = stableSeat;
			applicant.identity = MakeIdentity();
			applicant.displayName = name;
			return applicant;
		}

		// §9b: "two pending applicants for one seat", the pending-applicant spam bound (P15) and
		// "substitute inert - no peer id, no team, no snapshot, no authority - until the commit".
		int TestApplicantsAndBounds() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint holder;
			holder.connection = 61;
			ConfigureEndpoint(holder, "applicants", &unixNow);
			wire.Add(&holder);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, holder, record, unixNow, &error) != 0) {
				return Fail("could not drop a seat to moderate: " + error);
			}
			wire.ClearDelivered();

			// Two applicants for the one dropped seat: the plan's own floor, and both representable.
			if (!wire.SendRaw(71, MakeApplicant(0, 0x10, "first"), &error) ||
			    !wire.SendRaw(72, MakeApplicant(0, 0x11, "second"), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			for (NetPeerId connection : {71, 72}) {
				if (CountOf<NetH4ApplicantAck>(wire.Delivered(connection)) != 1) {
					return Fail("applicant " + std::to_string(connection) + " was not acknowledged");
				}
			}
			std::vector<NetH4ModerationSeat> view = wire.host.GetModerationView();
			if (view.size() != MakeSeatTable().size() || !view[0].substitutable || view[0].applicants.size() != 2) {
				return Fail("the moderation view does not show both applicants for the dropped seat");
			}
			if (view[0].applicants[0].displayName != "first" || view[0].applicants[1].displayName != "second" ||
			    view[0].applicants[0].approved || view[0].applicants[1].approved) {
				return Fail("the moderation view lost an applicant or approved one by itself");
			}
			if (view[1].substitutable || view[2].substitutable) {
				return Fail("a seat nobody dropped is offered for reassignment");
			}

			// Inert: an application moves nothing. No commit, no peer id, no credential, no reseat.
			NetPeerId seatHolder = 12345;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(0, seatHolder, generation, incarnation) || seatHolder != c_InvalidNetPeerId ||
			    generation != record.holderGeneration) {
				return Fail("an applicant disturbed the seat it applied for");
			}
			if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
				return Fail("an application moved the seat's credential");
			}
			if (!wire.host.TakePendingReseats().empty() || !wire.host.TakeCommits().empty()) {
				return Fail("an applicant earned authority before any host action");
			}
			if (wire.host.GetStats().applicantsRegistered != 2 || wire.host.GetApplicantCount() != 2) {
				return Fail("the applicant records were not what the host says they are");
			}
			const std::vector<NetH4SeatStatus> statuses = wire.host.GetSeatStatuses();
			if (statuses[0].applicants != 2 || statuses[0].substituting) {
				return Fail("the roster does not report the pending applicants");
			}

			// A third for the same seat is over the per-seat bound; a second from a connection that
			// already applied is over the per-connection bound. Both take the uniform delayed refusal.
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			const uint64_t refusedAtMs = wire.nowMs;
			if (!wire.SendRaw(73, MakeApplicant(0, 0x12, "third"), &error) ||
			    !wire.SendRaw(71, MakeApplicant(0, 0x13, "again"), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (CountOf<NetH4ApplicantAck>(wire.Delivered(73)) != 0) {
				return Fail("the per-seat bound admitted a third applicant");
			}
			if (CountOf<NetH4ApplicantAck>(wire.Delivered(71)) != 1) {
				return Fail("one connection was allowed to hold two applications");
			}
			if (CountOf<NetJoinRejected>(wire.Delivered(73)) != 0) {
				return Fail("a bounded applicant was refused before the release time");
			}
			wire.nowMs = refusedAtMs + NetReconnectAdmission::c_DenialReleaseMs;
			wire.DrainHostOutbound();
			if (CountOf<NetJoinRejected>(wire.Delivered(73)) != 1 || CountOf<NetJoinRejected>(wire.Delivered(71)) != 1) {
				return Fail("a bounded applicant was not refused on the uniform schedule");
			}
			if (wire.host.GetStats().applicantsRefused != 2 || wire.host.GetApplicantCount() != 2) {
				return Fail("a refused application still took a slot");
			}

			// A retransmitted application is the same one: it is answered, not counted twice.
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!wire.SendRaw(71, MakeApplicant(0, 0x10, "first"), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (CountOf<NetH4ApplicantAck>(wire.Delivered(71)) != 2 || wire.host.GetApplicantCount() != 2 ||
			    wire.host.GetStats().applicantsRegistered != 2) {
				return Fail("a retransmitted application opened a second record");
			}

			// The control: a seat nobody dropped, the CPU slot and a seat that does not exist all take
			// the SAME refusal, so applying reveals only what the roster already publishes.
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			const uint64_t controlAtMs = wire.nowMs;
			if (!wire.SendRaw(81, MakeApplicant(1, 0x20, "live"), &error) ||
			    !wire.SendRaw(82, MakeApplicant(2, 0x21, "cpu"), &error) ||
			    !wire.SendRaw(83, MakeApplicant(9, 0x22, "absent"), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			for (NetPeerId connection : {81, 82, 83}) {
				if (CountOf<NetH4ApplicantAck>(wire.Delivered(connection)) != 0) {
					return Fail("a seat that cannot be reassigned acknowledged an applicant");
				}
			}
			wire.nowMs = controlAtMs + NetReconnectAdmission::c_DenialReleaseMs;
			wire.DrainHostOutbound();
			const NetJoinRejected* reference = nullptr;
			for (NetPeerId connection : {81, 82, 83}) {
				const NetJoinRejected* refusal = LastOf<NetJoinRejected>(wire.Delivered(connection));
				if (refusal == nullptr) {
					return Fail("a refused applicant heard nothing at all");
				}
				if (reference == nullptr) {
					reference = refusal;
				} else if (!(*refusal == *reference)) {
					return Fail("two refused applications differ on the wire");
				}
			}

			// The records expire on P2's horizon, exactly like a provisional seat, and take nothing
			// with them.
			wire.nowMs += NetReconnectHost::c_ProvisionalExpiryMs + 1;
			wire.host.Tick(wire.nowMs);
			if (wire.host.GetApplicantCount() != 0 || wire.host.GetStats().applicantsExpired != 2) {
				return Fail("the applicant records did not expire on the P2 horizon");
			}
			if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
				return Fail("expiring an application touched the seat");
			}
			return 0;
		}

		// §9b: the substitution wire transaction - approval, provisional ticket, durable ack, the
		// seat-generation CAS and a replayable commit result - plus the ack-lost and
		// commit-result-lost windows and a delayed duplicate.
		int TestSubstitutionTransaction() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint holder;
			holder.connection = 61;
			ConfigureEndpoint(holder, "subst-holder", &unixNow);
			wire.Add(&holder);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, holder, record, unixNow, &error) != 0) {
				return Fail("could not drop a seat to substitute into: " + error);
			}
			wire.ClearDelivered();

			Endpoint substitute;
			substitute.connection = 91;
			ConfigureEndpoint(substitute, "substitute", &unixNow);
			wire.Add(&substitute);
			if (!substitute.client.BeginApplication(0, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the application did not settle: " + error);
			}
			if (substitute.client.GetState() != NetH4ClientState::Applied) {
				return Fail(std::string("the applicant did not settle on the host's list: ") + NetReconnectClientStateName(substitute.client.GetState()));
			}
			if (substitute.store.HasRecord()) {
				return Fail("an applicant was given a ticket before the host approved anything");
			}

			// The host approves. The credential is handed out but NOT installed, which is exactly what
			// makes the first COMMIT win rather than the first approval.
			if (wire.host.SubstituteApplicant(0, substitute.connection, wire.nowMs) != NetH4ModerationResult::Ok) {
				return Fail("the host could not approve the applicant");
			}
			if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
				return Fail("the approval installed a credential before the substitute committed");
			}
			if (!wire.host.HasSubstitution(0) || !wire.host.GetSeatStatuses()[0].substituting) {
				return Fail("the approval is not visible as a substitution in flight");
			}
			if (!wire.Pump(&error)) {
				return Fail(error);
			}
			if (substitute.client.GetState() != NetH4ClientState::Joined) {
				return Fail(std::string("the substitute did not commit: ") + NetReconnectClientStateName(substitute.client.GetState()));
			}
			if (CountOf<NetH4SubstitutionOffer>(wire.Delivered(substitute.connection)) != 1) {
				return Fail("the substitute did not get exactly one provisional ticket");
			}
			if (!substitute.store.HasRecord()) {
				return Fail("the substitute committed without a durable ticket");
			}
			if (substitute.client.GetAssignedPeerId() != MakeSeatTable()[0].peerId || substitute.client.GetIncarnation() != 1) {
				return Fail("the substitute did not take the seat's own peer id on a fresh incarnation");
			}
			if (wire.registry.GetActiveGeneration(0) != record.holderGeneration + 1) {
				return Fail("the commit did not install the substitute's holder generation");
			}
			if (wire.host.GetStats().substitutionsCommitted != 1 || wire.host.HasSubstitution(0)) {
				return Fail("the committed transaction did not close out");
			}
			NetPeerId seatHolder = c_InvalidNetPeerId;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(0, seatHolder, generation, incarnation) || seatHolder != substitute.connection ||
			    generation != record.holderGeneration + 1 || incarnation != 1) {
				return Fail("the seat did not change hands exactly once");
			}
			if (wire.host.GetApplicantCount() != 0) {
				return Fail("the committed substitute stayed on the applicant list");
			}

			// Commit result lost: a re-presented ack replays the success it already earned, even
			// carrying a mac that would never verify - the cache answers before anything is checked.
			const NetH4JoinCommitted* committed = LastOf<NetH4JoinCommitted>(wire.Delivered(substitute.connection));
			if (committed == nullptr) {
				return Fail("no commit reached the substitute");
			}
			const NetH4JoinCommitted firstCommit = *committed;
			const uint32_t replaysBefore = wire.host.GetStats().replayedResults;
			NetH4SubstitutionAck duplicate;
			duplicate.txId = firstCommit.txId;
			duplicate.stableSeat = 0;
			duplicate.holderGeneration = firstCommit.holderGeneration;
			duplicate.clientNonce = Ramp<16>(0x33);
			duplicate.mac = Ramp<32>(0x44);
			duplicate.stored = true;
			if (!wire.SendRaw(substitute.connection, duplicate, &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (wire.host.GetStats().replayedResults != replaysBefore + 1) {
				return Fail("a re-presented ack did not replay the cached commit");
			}
			const NetH4JoinCommitted* replay = LastOf<NetH4JoinCommitted>(wire.Delivered(substitute.connection));
			if (replay == nullptr || !(*replay == firstCommit)) {
				return Fail("the replayed commit was not byte-identical to the first");
			}
			if (wire.host.GetStats().substitutionsCommitted != 1 || wire.host.GetStats().incarnationsBound != 2) {
				return Fail("a duplicate ack ran a second substitution");
			}

			// A delayed duplicate arriving much later, still inside P17's window, replays too.
			wire.nowMs += NetReconnectTxCache::c_RetentionMs / 2;
			if (!wire.SendRaw(substitute.connection, duplicate, &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			const NetH4JoinCommitted* delayed = LastOf<NetH4JoinCommitted>(wire.Delivered(substitute.connection));
			if (delayed == nullptr || !(*delayed == firstCommit) || wire.host.GetStats().substitutionsCommitted != 1) {
				return Fail("a delayed duplicate ack was not answered from the cache");
			}

			// The credential the substitute persisted really is the seat's: it reclaims like any holder.
			NetH4TicketRecord substituteRecord;
			if (substitute.store.Load(unixNow, substituteRecord, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail("the substitute's ticket did not load: " + error);
			}
			if (substituteRecord.stableSeat != 0 || substituteRecord.holderGeneration != record.holderGeneration + 1) {
				return Fail("the substitute's record names the wrong seat or generation");
			}
			wire.host.NotifyDisconnect(substitute.connection, 200);
			substitute.connected = false;
			wire.Remove(substitute.connection);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			Endpoint returning;
			returning.connection = 92;
			ConfigureEndpoint(returning, "substitute", &unixNow);
			wire.Add(&returning);
			if (!returning.client.BeginReclaim(substituteRecord, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the substitute's own reclaim did not settle: " + error);
			}
			if (returning.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the substitute could not reclaim the seat it was given");
			}
			return 0;
		}

		// §9b's ack-lost window: the offer retransmits on P3's ladder, gives up inside its budget, and
		// an approval nobody answers expires without ever having touched the seat.
		int TestSubstitutionAckLost() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint holder;
			holder.connection = 61;
			ConfigureEndpoint(holder, "acklost", &unixNow);
			wire.Add(&holder);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, holder, record, unixNow, &error) != 0) {
				return Fail("could not drop a seat: " + error);
			}
			wire.ClearDelivered();
			// A raw connection, so nothing ever answers the offer.
			if (!wire.SendRaw(95, MakeApplicant(0, 0x30, "silent"), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (wire.host.SubstituteApplicant(0, 95, wire.nowMs) != NetH4ModerationResult::Ok) {
				return Fail("the host could not approve the silent applicant");
			}
			wire.DrainHostOutbound();
			if (CountOf<NetH4SubstitutionOffer>(wire.Delivered(95)) != 1) {
				return Fail("the approval did not send exactly one offer");
			}
			for (uint32_t attempt = 1; attempt <= NetReconnectHost::c_MaxRetransmits; ++attempt) {
				wire.nowMs += NetReconnectHost::c_RetransmitIntervalMs;
				wire.DrainHostOutbound();
				if (CountOf<NetH4SubstitutionOffer>(wire.Delivered(95)) != 1U + attempt) {
					return Fail("the offer ladder did not retransmit at P3's cadence");
				}
			}
			wire.nowMs += NetReconnectHost::c_RetransmitIntervalMs;
			wire.DrainHostOutbound();
			if (CountOf<NetH4SubstitutionOffer>(wire.Delivered(95)) != 1U + NetReconnectHost::c_MaxRetransmits) {
				return Fail("the offer ladder did not stop at its budget");
			}
			if (wire.host.GetStats().substitutionOfferRetransmits != NetReconnectHost::c_MaxRetransmits) {
				return Fail("the retransmits were not counted");
			}

			// P2 closes the window: the record goes, the substitute is told, and the seat is untouched.
			wire.nowMs += NetReconnectHost::c_ProvisionalExpiryMs;
			wire.DrainHostOutbound();
			if (wire.host.HasSubstitution(0)) {
				return Fail("an unacknowledged approval outlived its window");
			}
			if (CountOf<NetJoinRejected>(wire.Delivered(95)) != 1) {
				return Fail("the silent substitute was never told the approval lapsed");
			}
			if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
				return Fail("an unacknowledged approval changed the seat's credential");
			}
			NetPeerId seatHolder = 12345;
			uint32_t generation = 0;
			uint32_t incarnation = 0;
			if (!wire.host.GetSeatHolder(0, seatHolder, generation, incarnation) || seatHolder != c_InvalidNetPeerId ||
			    generation != record.holderGeneration) {
				return Fail("an unacknowledged approval left the seat somewhere else");
			}

			// The original holder still reclaims: the seat was never released for the substitute.
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			Endpoint returner;
			returner.connection = 96;
			ConfigureEndpoint(returner, "acklost", &unixNow);
			wire.Add(&returner);
			if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the holder's reclaim did not settle: " + error);
			}
			if (returner.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the original holder lost its seat to an approval that never committed");
			}
			return 0;
		}

		// §9b's cleanup rules: host-cancel, substitute-disappears-before-ack and a persistence failure
		// each invalidate AND remove the provisional record, and none of them releases the seat first.
		int TestSubstitutionCleanup() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			Endpoint holder;
			holder.connection = 61;
			ConfigureEndpoint(holder, "cleanup", &unixNow);
			wire.Add(&holder);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, holder, record, unixNow, &error) != 0) {
				return Fail("could not drop a seat: " + error);
			}
			wire.ClearDelivered();

			// Host cancel.
			if (!wire.SendRaw(101, MakeApplicant(0, 0x40, "cancelled"), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (wire.host.SubstituteApplicant(0, 101, wire.nowMs) != NetH4ModerationResult::Ok) {
				return Fail("the host could not approve the applicant");
			}
			wire.DrainHostOutbound();
			const NetH4SubstitutionOffer* offer = LastOf<NetH4SubstitutionOffer>(wire.Delivered(101));
			if (offer == nullptr) {
				return Fail("no offer was delivered to cancel");
			}
			const NetH4SubstitutionOffer cancelled = *offer;
			if (wire.host.SubstituteApplicant(0, 101, wire.nowMs) != NetH4ModerationResult::SubstitutionInFlight) {
				return Fail("the host was allowed to approve a second substitution for one seat");
			}
			if (wire.host.CancelSubstitution(0, wire.nowMs) != NetH4ModerationResult::Ok) {
				return Fail("the host could not withdraw its own approval");
			}
			if (wire.host.HasSubstitution(0)) {
				return Fail("the cancelled approval was not removed");
			}
			wire.DrainHostOutbound();
			if (CountOf<NetJoinRejected>(wire.Delivered(101)) != 1) {
				return Fail("the substitute was not told the approval was withdrawn");
			}
			if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
				return Fail("a cancel changed the seat's credential");
			}
			if (wire.host.CancelSubstitution(0, wire.nowMs) != NetH4ModerationResult::NoSubstitutionPending) {
				return Fail("cancelling twice did not say there was nothing pending");
			}
			// An ack already in flight replays the refusal instead of finding nothing and hearing
			// nothing: the terminal result was cached before the record went.
			NetH4SubstitutionAck late;
			late.txId = cancelled.txId;
			late.stableSeat = cancelled.stableSeat;
			late.holderGeneration = cancelled.holderGeneration;
			late.clientNonce = Ramp<16>(0x41);
			late.mac = Ramp<32>(0x42);
			late.stored = true;
			if (!wire.SendRaw(101, late, &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (CountOf<NetJoinRejected>(wire.Delivered(101)) != 2 || CountOf<NetH4JoinCommitted>(wire.Delivered(101)) != 0) {
				return Fail("a late ack for a cancelled approval was not answered from the cache");
			}
			if (wire.host.GetStats().substitutionsCommitted != 0) {
				return Fail("a cancelled substitution committed anyway");
			}

			// Substitute disappears before its ack.
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!wire.SendRaw(111, MakeApplicant(0, 0x43, "vanishing"), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (wire.host.SubstituteApplicant(0, 111, wire.nowMs) != NetH4ModerationResult::Ok) {
				return Fail("the host could not approve the second applicant");
			}
			wire.DrainHostOutbound();
			if (wire.host.NotifyDisconnect(111, 300) != NetH4DisconnectOutcome::Unknown) {
				return Fail("an applicant's disconnect was adjudicated as a seat loss");
			}
			if (wire.host.HasSubstitution(0)) {
				return Fail("a vanished substitute left its approval behind");
			}
			// Its application goes with it. The one the host merely CANCELLED stays: that player is
			// still asking, and the host may pick it next.
			const std::vector<NetH4ApplicantView> pending = wire.host.GetModerationView()[0].applicants;
			if (std::any_of(pending.begin(), pending.end(), [](const NetH4ApplicantView& applicant) { return applicant.connection == 111; })) {
				return Fail("a vanished substitute left its application behind");
			}
			if (pending.size() != 1 || pending.front().connection != 101 || pending.front().approved) {
				return Fail("the cancelled applicant did not stay on the list, unapproved");
			}
			if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
				return Fail("a vanished substitute took the seat's credential with it");
			}

			// Persistence failure blocks a substitution exactly as it blocks a first join.
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!wire.SendRaw(121, MakeApplicant(0, 0x44, "unstorable"), &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (wire.host.SubstituteApplicant(0, 121, wire.nowMs) != NetH4ModerationResult::Ok) {
				return Fail("the host could not approve the third applicant");
			}
			wire.DrainHostOutbound();
			const NetH4SubstitutionOffer* unstorable = LastOf<NetH4SubstitutionOffer>(wire.Delivered(121));
			if (unstorable == nullptr) {
				return Fail("no offer reached the third applicant");
			}
			NetH4SubstitutionAck refused;
			refused.txId = unstorable->txId;
			refused.stableSeat = unstorable->stableSeat;
			refused.holderGeneration = unstorable->holderGeneration;
			refused.stored = false;
			if (!wire.SendRaw(121, refused, &error)) {
				return Fail(error);
			}
			wire.DrainHostOutbound();
			if (wire.host.HasSubstitution(0) || wire.host.GetStats().substitutionsCommitted != 0) {
				return Fail("a substitute that could not persist its ticket still took the seat");
			}
			if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
				return Fail("a failed persistence still moved the seat's credential");
			}

			// After all of it the original holder still reclaims: the seat was never released.
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			Endpoint returner;
			returner.connection = 131;
			ConfigureEndpoint(returner, "cleanup", &unixNow);
			wire.Add(&returner);
			if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the holder's reclaim did not settle: " + error);
			}
			if (returner.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the original holder lost its seat to a cleaned-up substitution");
			}
			return 0;
		}

		// §9b: valid returner versus a simultaneous host reassignment, BOTH orders. The first COMMIT
		// wins; the loser is told precisely why, and only a loser that proves possession is told at all.
		int TestSubstitutionRaceBothOrders() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			uint64_t unixNow = 1'700'000'000'000ULL;

			// Order one: the returner commits while an approval is outstanding.
			{
				Wire wire;
				ConfigureWire(wire);
				Endpoint holder;
				holder.connection = 61;
				ConfigureEndpoint(holder, "race-a", &unixNow);
				wire.Add(&holder);
				NetH4TicketRecord record;
				if (SeatAndDrop(wire, holder, record, unixNow, &error) != 0) {
					return Fail("could not drop a seat: " + error);
				}
				wire.ClearDelivered();
				Endpoint substitute;
				substitute.connection = 141;
				ConfigureEndpoint(substitute, "race-a-substitute", &unixNow);
				wire.Add(&substitute);
				if (!substitute.client.BeginApplication(0, wire.nowMs, &error) || !wire.Pump(&error)) {
					return Fail("the application did not settle: " + error);
				}
				if (wire.host.SubstituteApplicant(0, substitute.connection, wire.nowMs) != NetH4ModerationResult::Ok) {
					return Fail("the host could not approve the applicant");
				}
				// The substitute persists its ticket and answers - and the answer is LOST in flight,
				// which is the whole point: it is a genuine, correctly signed ack that arrives late.
				std::vector<NetPayload> heldReplies;
				if (!wire.Step(substitute, heldReplies, &error)) {
					return Fail("the approval did not reach the substitute: " + error);
				}
				if (heldReplies.size() != 1 || !std::holds_alternative<NetH4SubstitutionAck>(heldReplies.front())) {
					return Fail("the approved substitute did not produce exactly one ack to lose");
				}
				const NetPayload lostAck = heldReplies.front();
				if (substitute.client.GetState() != NetH4ClientState::Substituting || !substitute.store.HasRecord()) {
					return Fail("the substitute did not persist its provisional ticket before answering");
				}
				// Out of the pump, so its retransmits cannot deliver the ack behind the test's back.
				wire.Remove(substitute.connection);
				wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
				Endpoint returner;
				returner.connection = 142;
				ConfigureEndpoint(returner, "race-a", &unixNow);
				wire.Add(&returner);
				if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
					return Fail("the reclaim did not settle: " + error);
				}
				if (returner.client.GetState() != NetH4ClientState::Joined) {
					return Fail("the returner did not win by committing first");
				}
				if (wire.host.HasSubstitution(0)) {
					return Fail("the losing approval was not invalidated and removed");
				}
				if (wire.host.GetStats().substitutionsSuperseded != 1) {
					return Fail("the superseded approval was not recorded as such");
				}
				if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
					return Fail("the loser's generation was installed anyway");
				}
				// The substitute's correctly signed ack lands after the race is over. It is answered
				// precisely: it is host-approved and holds host-issued material, so there is nothing
				// here it did not already know.
				if (!wire.SendRaw(substitute.connection, lostAck, &error)) {
					return Fail(error);
				}
				wire.DrainHostOutbound();
				if (wire.host.GetStats().substitutionAckFailures != 0) {
					return Fail("the losing ack was read as a bad proof rather than a lost race");
				}
				const NetJoinRejected* refusal = LastOf<NetJoinRejected>(wire.Delivered(substitute.connection));
				if (refusal == nullptr || refusal->mismatchKey != "substitution" ||
				    refusal->expected != std::string(NetH4DenialReasonName(NetH4DenialReason::SubstitutionSuperseded))) {
					return Fail("the losing substitute was not told precisely why it lost");
				}
				if (CountOf<NetH4JoinCommitted>(wire.Delivered(substitute.connection)) != 0) {
					return Fail("the losing substitute was committed anyway");
				}
				if (wire.registry.GetActiveGeneration(0) != record.holderGeneration) {
					return Fail("the losing substitute's credential was installed after it lost");
				}
				NetPeerId seatHolder = c_InvalidNetPeerId;
				uint32_t generation = 0;
				uint32_t incarnation = 0;
				if (!wire.host.GetSeatHolder(0, seatHolder, generation, incarnation) || seatHolder != returner.connection) {
					return Fail("the seat did not end up with the winner");
				}
			}

			// Order two: the substitute commits first, and the original holder comes back afterwards.
			{
				Wire wire;
				ConfigureWire(wire);
				Endpoint holder;
				holder.connection = 61;
				ConfigureEndpoint(holder, "race-b-holder", &unixNow);
				wire.Add(&holder);
				NetH4TicketRecord record;
				if (SeatAndDrop(wire, holder, record, unixNow, &error) != 0) {
					return Fail("could not drop a seat: " + error);
				}
				wire.ClearDelivered();
				Endpoint substitute;
				substitute.connection = 151;
				ConfigureEndpoint(substitute, "race-b-substitute", &unixNow);
				wire.Add(&substitute);
				if (!substitute.client.BeginApplication(0, wire.nowMs, &error) || !wire.Pump(&error)) {
					return Fail("the application did not settle: " + error);
				}
				if (wire.host.SubstituteApplicant(0, substitute.connection, wire.nowMs) != NetH4ModerationResult::Ok ||
				    !wire.Pump(&error)) {
					return Fail("the substitution did not settle: " + error);
				}
				if (substitute.client.GetState() != NetH4ClientState::Joined) {
					return Fail("the substitute did not win by committing first");
				}

				// The ex-holder returns. Its generation is superseded, so it gets a real challenge -
				// indistinguishable from a live seat - and, having proved it, is told what happened.
				wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
				const uint64_t reclaimAtMs = wire.nowMs;
				Endpoint exHolder;
				exHolder.connection = 152;
				ConfigureEndpoint(exHolder, "race-b-holder", &unixNow);
				wire.Add(&exHolder);
				if (!exHolder.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
					return Fail("the late reclaim did not settle: " + error);
				}
				if (exHolder.client.GetState() == NetH4ClientState::Joined) {
					return Fail("a superseded ticket took the seat back");
				}
				if (CountOf<NetH4Challenge>(wire.Delivered(exHolder.connection)) != 1) {
					return Fail("a superseded generation did not get the same challenge a live one gets");
				}
				if (CountOf<NetJoinRejected>(wire.Delivered(exHolder.connection)) != 0) {
					return Fail("the refusal arrived before its release time");
				}
				wire.nowMs = reclaimAtMs + NetReconnectAdmission::c_DenialReleaseMs;
				wire.DrainHostOutbound();
				const NetJoinRejected* told = LastOf<NetJoinRejected>(wire.Delivered(exHolder.connection));
				if (told == nullptr || told->rejectReason != NetRejectReason::SeatReassigned) {
					return Fail(std::string("the superseded holder was not told the seat was reassigned: ") +
					            (told == nullptr ? "nothing was delivered" : NetProtocol::RejectReasonName(told->rejectReason)) +
					            " synthetic=" + std::to_string(wire.host.GetAdmission().GetSyntheticChallenges()) +
					            " retired=" + std::to_string(wire.registry.HasRetiredGeneration(0, record.holderGeneration)) +
					            " active=" + std::to_string(wire.registry.GetActiveGeneration(0)) +
					            " client=" + NetReconnectClientStateName(exHolder.client.GetState()));
				}
				if (wire.host.GetStats().reassignedReclaimsRefused != 1) {
					return Fail("the precise refusal was not recorded");
				}
				NetPeerId seatHolder = c_InvalidNetPeerId;
				uint32_t generation = 0;
				uint32_t incarnation = 0;
				if (!wire.host.GetSeatHolder(0, seatHolder, generation, incarnation) || seatHolder != substitute.connection ||
				    generation != record.holderGeneration + 1) {
					return Fail("the late reclaim disturbed the seat it lost");
				}

				// The oracle control: the SAME superseded seat and generation, without the credential,
				// reads exactly like a seat that never existed.
				wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
				const uint64_t probeAtMs = wire.nowMs;
				NetH4Reclaim probe;
				probe.txId = Ramp<16>(0x60);
				probe.epoch = record.epoch;
				probe.stableSeat = 0;
				probe.holderGeneration = record.holderGeneration;
				probe.identity = MakeIdentity();
				probe.displayName = "probe";
				NetH4Reclaim unknown = probe;
				unknown.txId = Ramp<16>(0x61);
				unknown.stableSeat = 9;
				if (!wire.SendRaw(161, probe, &error) || !wire.SendRaw(162, unknown, &error)) {
					return Fail(error);
				}
				wire.DrainHostOutbound();
				if (CountOf<NetH4Challenge>(wire.Delivered(161)) != 1 || CountOf<NetH4Challenge>(wire.Delivered(162)) != 1) {
					return Fail("the two probes did not each get exactly one challenge");
				}
				NetH4Proof badProof;
				badProof.txId = probe.txId;
				badProof.epoch = record.epoch;
				badProof.stableSeat = 0;
				badProof.holderGeneration = record.holderGeneration;
				badProof.clientNonce = Ramp<16>(0x62);
				badProof.mac = Ramp<32>(0x63);
				if (!wire.SendRaw(161, badProof, &error)) {
					return Fail(error);
				}
				wire.nowMs = probeAtMs + NetReconnectAdmission::c_DenialReleaseMs;
				wire.DrainHostOutbound();
				const NetJoinRejected* probeRefusal = LastOf<NetJoinRejected>(wire.Delivered(161));
				const NetJoinRejected* unknownRefusal = LastOf<NetJoinRejected>(wire.Delivered(162));
				if (probeRefusal == nullptr || unknownRefusal == nullptr) {
					return Fail("a probe heard nothing at all");
				}
				if (!(*probeRefusal == *unknownRefusal)) {
					return Fail("a superseded seat answered without the credential reads differently from an unknown one");
				}
				if (wire.host.GetStats().reassignedReclaimsRefused != 1) {
					return Fail("a claimant that proved nothing was told the seat was reassigned");
				}

				// Past P2 the retired credential is gone, so nothing can be answered precisely again.
				wire.nowMs += NetReconnectHost::c_ProvisionalExpiryMs;
				wire.host.Tick(wire.nowMs);
				if (wire.registry.HasRetiredGeneration(0, record.holderGeneration)) {
					return Fail("the retired credential outlived the window it is kept for");
				}
			}
			return 0;
		}

		// §8 for a substitute - it receives the ledgered ownership from resumed tick 1 - and the
		// moderation verbs' own answers.
		int TestSubstituteOwnershipAndModeration() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			g_Census = {
			    {101, 1, 2, true},
			    {102, 1, 2, true},
			    {103, 1, 2, true},
			    {201, 2, 3, true},
			};
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			wire.host.SetDropOwnershipSource(&CensusSource, nullptr);
			Endpoint holder;
			holder.connection = 61;
			ConfigureEndpoint(holder, "subst-ledger", &unixNow);
			wire.Add(&holder);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, holder, record, unixNow, &error) != 0) {
				return Fail("could not drop a seat: " + error);
			}
			wire.ClearDelivered();

			// The moderation verbs answer for themselves before anything is approved.
			if (wire.host.WaitForSeat(0) != NetH4ModerationResult::Ok) {
				return Fail("waiting for a dropped seat was refused");
			}
			if (wire.host.WaitForSeat(9) != NetH4ModerationResult::UnknownSeat ||
			    wire.host.WaitForSeat(1) != NetH4ModerationResult::SeatNotSubstitutable) {
				return Fail("the wait verb did not answer for a seat it cannot moderate");
			}
			if (wire.host.SubstituteApplicant(9, 171, wire.nowMs) != NetH4ModerationResult::UnknownSeat ||
			    wire.host.SubstituteApplicant(1, 171, wire.nowMs) != NetH4ModerationResult::SeatNotSubstitutable ||
			    wire.host.SubstituteApplicant(0, 171, wire.nowMs) != NetH4ModerationResult::UnknownApplicant ||
			    wire.host.CancelSubstitution(0, wire.nowMs) != NetH4ModerationResult::NoSubstitutionPending) {
				return Fail("a moderation verb accepted something it should have named");
			}

			// One actor dies and one changes team while the seat is empty; neither comes back.
			g_Census = {
			    {101, 1, 0, true},
			    {102, 1, 0, false},
			    {103, 2, 0, true},
			    {201, 2, 3, true},
			};
			Endpoint substitute;
			substitute.connection = 171;
			ConfigureEndpoint(substitute, "subst-ledger-in", &unixNow);
			wire.Add(&substitute);
			if (!substitute.client.BeginApplication(0, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the application did not settle: " + error);
			}
			if (wire.host.SubstituteApplicant(0, substitute.connection, wire.nowMs) != NetH4ModerationResult::Ok || !wire.Pump(&error)) {
				return Fail("the substitution did not settle: " + error);
			}
			if (substitute.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the substitute did not commit");
			}
			const std::vector<NetGameReseat> reseats = wire.host.TakePendingReseats();
			if (reseats.size() != 1) {
				return Fail("the committed substitution did not produce exactly one reseat");
			}
			if (reseats.front().team != 1 || reseats.front().newOwnerPeerId != MakeSeatTable()[0].lockstepPeerId ||
			    reseats.front().newOwnerPeerId == MakeSeatTable()[0].peerId) {
				return Fail("the substitute's reseat did not address the seat it took");
			}
			if (reseats.front().actorUIDs != std::vector<int64_t>{101}) {
				return Fail("the substitute did not receive exactly the surviving ledgered actors");
			}

			// The control: a seat with no ledger entry earns a substitute no authority at all.
			wire.host.SetLiveMatch(true);
			Endpoint second;
			second.connection = 181;
			ConfigureEndpoint(second, "subst-ledger-none", &unixNow);
			wire.Add(&second);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			// Seat 1 was never held, so a clean leave is what makes it empty rather than a drop.
			wire.host.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			Endpoint leaver;
			leaver.connection = 182;
			ConfigureEndpoint(leaver, "subst-leaver", &unixNow);
			wire.Add(&leaver);
			wire.host.SetLiveMatch(false);
			if (!leaver.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the second seat's join did not settle: " + error);
			}
			if (leaver.client.GetState() != NetH4ClientState::Joined) {
				return Fail("the second seat was never taken");
			}
			wire.host.SetLiveMatch(true);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!leaver.client.BeginLeave(wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the clean leave did not settle: " + error);
			}
			if (!wire.host.IsSeatClosed(1)) {
				return Fail("a clean mid-match leave did not close the seat");
			}
			if (!second.client.BeginApplication(1, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the application for the vacated seat did not settle: " + error);
			}
			if (second.client.GetState() != NetH4ClientState::Applied) {
				return Fail("a seat left empty by a clean leave refused an applicant");
			}
			if (wire.host.SubstituteApplicant(1, second.connection, wire.nowMs) != NetH4ModerationResult::Ok || !wire.Pump(&error)) {
				return Fail("the second substitution did not settle: " + error);
			}
			if (second.client.GetState() != NetH4ClientState::Joined) {
				return Fail("a substitute could not take a seat somebody cleanly left");
			}
			if (!wire.host.TakePendingReseats().empty()) {
				return Fail("a seat with no ledger entry handed a substitute somebody's actors");
			}
			return 0;
		}
	} // namespace

		// A lobby member whose process is gone sends nothing and closes nothing: the host reaps it on
		// its own heartbeat timeout. That close is ours, so no transport event ever reaches the
		// admission plane, and before this the seat stayed committed to a peer that no longer existed -
		// every replacement was then refused a full lobby, which is what the 4-peer drop lane measured.
		int TestReapedLobbySeatReturnsToThePool() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint16_t port = 42160;
			LoopbackTransport hostTransport;
			LoopbackTransport firstTransport;
			LoopbackTransport secondTransport;
			NetSession host;
			NetSession first;
			NetSession second;
			NetSeatAuthRegistry registry;
			registry.BeginHostedSession();
			NetReconnectHost admission;
			admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
			// One human seat, so the replacement can only be admitted if the reaped seat came back.
			admission.SetSeatTable({{0, 1, 1, false, 2, false}, {1, 0, 3, true, 0, false}}, NetMatchMode::PvPSkirmish);
			NetReconnectTicketStore firstStore;
			firstStore.SetPath(StorePath("reap-first"));
			NetReconnectClient firstClient;
			uint64_t unixNow = 1'700'000'000'000ULL;
			firstClient.Configure(&firstStore, MakeIdentity(), "Player");
			firstClient.SetUnixClock(&FixedUnixClock, &unixNow);
			firstClient.SetHostContext("loopback", MakeHash(5));
			host.SetReconnectHost(&admission);
			first.SetReconnectClient(&firstClient);
			NetSessionConfig hostConfig = MakeSessionConfig(port, 101, "Host");
			hostConfig.maxPeers = 2;
			// The transport says nothing about a close we make ourselves, which is how GNS behaved: the
			// seat has to come back because the session hands the peer over, not because the wire did.
			LoopbackTransportConfig silentClose;
			silentClose.silentLocalDisconnect = true;
			hostTransport.SetFaultConfig(silentClose);
			if (!host.StartHost(hostTransport, hostConfig, &error) ||
			    !first.StartClient(firstTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not seat the first member: " + error);
			}
			uint64_t nowMs = 0;
			for (; nowMs <= 600; nowMs += 10) {
				host.Tick(nowMs);
				first.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				firstTransport.AdvanceTimeMs(10);
			}
			if (firstClient.GetState() != NetH4ClientState::Joined || host.GetReadyPeerCount() != 1) {
				return Fail("the first member never committed the only seat");
			}

			// Its process dies: it stops answering and it never closes the socket. Only the host runs.
			const uint64_t silentFrom = nowMs;
			for (const uint64_t until = silentFrom + 2 * hostConfig.timeoutMs; nowMs <= until; nowMs += 10) {
				host.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
			}
			if (host.GetStats().timeouts == 0) {
				return Fail("the host never reaped the silent member");
			}
			const std::vector<NetH4SeatStatus> seats = admission.GetSeatStatuses();
			const auto reaped = std::find_if(seats.begin(), seats.end(), [](const NetH4SeatStatus& seat) { return seat.stableSeat == 0; });
			if (reaped == seats.end() || reaped->committed) {
				return Fail("the reaped member's seat is still committed to a peer that is gone");
			}
			// It has to be the reap that released it, not a later sweep: the session hands the peer over.
			if (admission.GetStats().seatsReleased != 1 || admission.GetStats().seatsClosedByLeave != 0) {
				return Fail("the seat came back by some route other than the reap: released=" +
				            std::to_string(admission.GetStats().seatsReleased) +
				            " byLeave=" + std::to_string(admission.GetStats().seatsClosedByLeave));
			}

			// The replacement: it can only be admitted onto the one seat the reap released.
			NetReconnectTicketStore secondStore;
			secondStore.SetPath(StorePath("reap-second"));
			NetReconnectClient secondClient;
			secondClient.Configure(&secondStore, MakeIdentity(), "Replacement");
			secondClient.SetUnixClock(&FixedUnixClock, &unixNow);
			secondClient.SetHostContext("loopback", MakeHash(5));
			second.SetReconnectClient(&secondClient);
			if (!second.StartClient(secondTransport, "loopback", MakeSessionConfig(port, 303, "Replacement"), &error)) {
				return Fail("the replacement could not connect: " + error);
			}
			for (const uint64_t until = nowMs + 1200; nowMs <= until; nowMs += 10) {
				host.Tick(nowMs);
				second.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				secondTransport.AdvanceTimeMs(10);
			}
			if (secondClient.GetState() != NetH4ClientState::Joined) {
				return Fail(std::string("the replacement was refused the reaped seat: ") + second.BuildRejectText());
			}
			if (host.GetReadyPeerCount() != 1) {
				return Fail("the host did not end up with exactly the replacement seated");
			}
			return 0;
		}

		// A7 defect B: mid-match the coordinator owns the transport queue, so the session is driven
		// through TickAdmissionPlane alone. P14's bound on half-open connections is only useful if that
		// path expires them: eight silent sockets otherwise hold the bound forever and no returner can
		// reclaim its seat. Driven exactly as the live match drives it - polled and injected, never Tick.
		int TestLiveAdmissionExpiresSilentConnections() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint16_t port = 42140;
			LoopbackTransport hostTransport;
			LoopbackTransport holderTransport;
			NetSession host;
			NetSession holder;
			NetSeatAuthRegistry registry;
			registry.BeginHostedSession();
			NetReconnectHost admission;
			admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
			admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			NetReconnectTicketStore store;
			store.SetPath(StorePath("silent"));
			NetReconnectClient holderClient;
			uint64_t unixNow = 1'700'000'000'000ULL;
			holderClient.Configure(&store, MakeIdentity(), "Player");
			holderClient.SetUnixClock(&FixedUnixClock, &unixNow);
			holderClient.SetHostContext("loopback", MakeHash(5));
			host.SetReconnectHost(&admission);
			holder.SetReconnectClient(&holderClient);
			NetSessionConfig hostConfig = MakeSessionConfig(port, 101, "Host");
			hostConfig.maxPeers = 1;
			if (!host.StartHost(hostTransport, hostConfig, &error) ||
			    !holder.StartClient(holderTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not seat the holder: " + error);
			}
			uint64_t nowMs = 0;
			for (; nowMs <= 600; nowMs += 10) {
				host.Tick(nowMs);
				holder.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				holderTransport.AdvanceTimeMs(10);
			}
			if (holderClient.GetState() != NetH4ClientState::Joined || host.GetReadyPeerCount() != 1) {
				return Fail("the holder never committed the only seat");
			}

			// The match is live: from here the session sees only what the round hands it.
			admission.SetLiveMatch(true);
			const uint64_t liveFromMs = nowMs;
			auto pumpLive = [&](uint64_t untilMs) {
				for (; nowMs <= untilMs; nowMs += 10) {
					for (const NetTransportEvent& event: hostTransport.PollEvents()) {
						host.InjectEvent(event, nowMs);
					}
					host.TickAdmissionPlane(nowMs);
					hostTransport.AdvanceTimeMs(10);
					holderTransport.AdvanceTimeMs(10);
				}
			};

			// Eight sockets connect and say nothing, which is exactly P14's bound.
			std::vector<std::unique_ptr<LoopbackTransport>> silent;
			for (uint32_t index = 0; index < NetSession::c_MaxUnauthenticatedPeers; ++index) {
				auto transport = std::make_unique<LoopbackTransport>();
				if (!transport->Connect("loopback", port, &error)) {
					return Fail("a silent connection could not be made: " + error);
				}
				silent.push_back(std::move(transport));
			}
			pumpLive(nowMs + 200);
			if (host.GetUnauthenticatedPeerCount() != NetSession::c_MaxUnauthenticatedPeers) {
				return Fail("the silent connections did not reach the session through the live path: " +
				            std::to_string(host.GetUnauthenticatedPeerCount()));
			}

			// Inside P14 they are still the host's problem to keep.
			pumpLive(liveFromMs + MakeSessionConfig(port, 0, "Host").timeoutMs - 500);
			if (host.GetUnauthenticatedPeerCount() != NetSession::c_MaxUnauthenticatedPeers) {
				return Fail("a silent connection was expired before P14's deadline");
			}
			// Past it they must go, or the bound is permanent.
			pumpLive(liveFromMs + MakeSessionConfig(port, 0, "Host").timeoutMs + 1000);
			if (host.GetUnauthenticatedPeerCount() != 0) {
				return Fail("the live path never expired the silent connections: " +
				            std::to_string(host.GetUnauthenticatedPeerCount()) + " still held the bound");
			}
			// The control that matters: the live holder's traffic rides the round, so its own receive
			// clock is stale by design and it must NOT be timed out with them.
			if (host.GetReadyPeerCount() != 1) {
				return Fail("expiring the silent connections dropped the live player too");
			}

			// The point of the bound: a returner can now be admitted.
			NetH4TicketRecord record;
			if (store.Load(unixNow, record, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail(error);
			}
			holderTransport.Stop();
			pumpLive(nowMs + 200);
			LoopbackTransport returnerTransport;
			NetSession returner;
			NetReconnectClient returnerClient;
			returnerClient.Configure(&store, MakeIdentity(), "Player");
			returnerClient.SetUnixClock(&FixedUnixClock, &unixNow);
			returnerClient.SetHostContext("loopback", MakeHash(5));
			returner.SetReconnectClient(&returnerClient);
			if (!returner.StartClient(returnerTransport, "loopback", MakeSessionConfig(port, 303, "Player"), &error)) {
				return Fail("the returner could not connect: " + error);
			}
			for (const uint64_t until = nowMs + 4000; nowMs <= until; nowMs += 10) {
				for (const NetTransportEvent& event: hostTransport.PollEvents()) {
					host.InjectEvent(event, nowMs);
				}
				host.TickAdmissionPlane(nowMs);
				returner.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				returnerTransport.AdvanceTimeMs(10);
			}
			if (returnerClient.GetState() != NetH4ClientState::Joined || !returnerClient.UsedStoredTicket()) {
				return Fail(std::string("the returner was not admitted after the bound cleared: ") +
				            NetReconnectClientStateName(returnerClient.GetState()) + " reject=" + returner.BuildRejectText());
			}
			return 0;
		}

		// A7 defect A: every admission deadline is real time. The pump that feeds them runs once per sim
		// tick from the game loop AND again from inside the lockstep wait, so a clock that adds a fixed
		// step per pump runs at whatever rate the frame happens to pump - halving P2 under a normal
		// double pump, and stopping dead across a pause. The seat hold is measured here in elapsed
		// milliseconds against a real NetReconnectHost, driven exactly as the service drives it.
		int TestAdmissionClockIsElapsedTime() {
			// How long a seat survives, in elapsed ms, when the plane is ticked `pumpsPerStep` times per
			// 10 ms of elapsed time through the given clock discipline.
			auto holdEndsAtMs = [](bool countPumps, uint32_t pumpsPerStep, uint64_t stepMs, uint64_t giveUpMs) -> uint64_t {
				ScriptedAuthCrypto crypto;
				ScopedTestCrypto scope(&crypto);
				std::string error;
				if (!ResetLaneDirectory(&error)) {
					return UINT64_MAX;
				}
				uint64_t unixNow = 1'700'000'000'000ULL;
				Wire wire;
				ConfigureWire(wire);
				Endpoint player;
				player.connection = 111;
				ConfigureEndpoint(player, "clock", &unixNow);
				wire.Add(&player);
				if (!player.client.BeginNewJoin(wire.nowMs, &error) || !wire.Pump(&error)) {
					return UINT64_MAX;
				}
				const std::vector<NetH4Seat> seats = MakeSeatTable();
				wire.host.SetLiveMatch(true);

				NetAdmissionClock clock;
				uint64_t steadyMs = 9'000'000ULL; // An arbitrary origin: elapsed time, never the raw clock.
				clock.Start(steadyMs);
				uint64_t pumpCounter = 0;
				auto planeNowMs = [&]() -> uint64_t {
					// The defect, exactly: a step added per pump instead of the elapsed time.
					return countPumps ? (pumpCounter += 15) : clock.NowMs(steadyMs);
				};
				wire.host.Tick(planeNowMs());
				wire.host.NotifyDisconnect(player.connection, 120);
				player.connected = false;
				for (uint64_t elapsedMs = 0; elapsedMs <= giveUpMs; elapsedMs += stepMs) {
					for (uint32_t pump = 0; pump < pumpsPerStep; ++pump) {
						wire.host.Tick(planeNowMs());
						if (!wire.host.IsSeatHeldForReclaim(seats[0].lockstepPeerId)) {
							return elapsedMs;
						}
					}
					steadyMs += stepMs;
				}
				return giveUpMs + 1;
			};

			const uint64_t windowMs = NetReconnectHost::c_ProvisionalExpiryMs;
			const uint64_t giveUpMs = windowMs * 3;

			// One pump per 10 ms of elapsed time: the seat is held for its window and no longer.
			const uint64_t singleMs = holdEndsAtMs(false, 1, 10, giveUpMs);
			if (singleMs < windowMs || singleMs > windowMs + 20) {
				return Fail("a singly-pumped seat hold did not last P2: " + std::to_string(singleMs) + " ms");
			}
			// Two pumps a tick is what the game loop and the lockstep wait actually do together; the
			// window must not move at all.
			const uint64_t doubleMs = holdEndsAtMs(false, 2, 10, giveUpMs);
			if (doubleMs != singleMs) {
				return Fail("double pumping moved the P2 window to " + std::to_string(doubleMs) + " ms");
			}
			// A stall pumps the plane hundreds of times inside a few milliseconds.
			const uint64_t stalledMs = holdEndsAtMs(false, 200, 10, giveUpMs);
			if (stalledMs != singleMs) {
				return Fail("a stall moved the P2 window to " + std::to_string(stalledMs) + " ms");
			}
			// A pause pumps rarely over a long time; the window must not stretch either.
			const uint64_t pausedMs = holdEndsAtMs(false, 1, 5000, giveUpMs);
			if (pausedMs < windowMs || pausedMs > windowMs + 5000) {
				return Fail("a pause stretched the P2 window to " + std::to_string(pausedMs) + " ms");
			}

			// The negative control: counting pumps is the defect, and it must still measure as one.
			const uint64_t countedDoubleMs = holdEndsAtMs(true, 2, 10, giveUpMs);
			if (countedDoubleMs >= windowMs) {
				return Fail("the pump-counting control did not shorten the window, so this case proves nothing");
			}
			const uint64_t countedPausedMs = holdEndsAtMs(true, 1, 5000, giveUpMs);
			if (countedPausedMs <= giveUpMs) {
				return Fail("the pump-counting control expired a paused hold, so this case proves nothing");
			}

			// The clock itself: an origin is not part of the answer, and it never runs backwards.
			NetAdmissionClock clock;
			if (clock.IsStarted() || clock.NowMs(1234) != 0) {
				return Fail("an unstarted admission clock reported time");
			}
			clock.Start(9'000'000ULL);
			if (!clock.IsStarted() || clock.NowMs(9'000'000ULL) != 0 || clock.NowMs(9'020'000ULL) != 20'000ULL) {
				return Fail("the admission clock did not report elapsed milliseconds");
			}
			if (clock.NowMs(8'999'000ULL) != 0) {
				return Fail("the admission clock ran backwards");
			}
			return 0;
		}

		// A minimal two-peer match config, enough for a lobby round to start.
		NetMatchConfig MakeLobbyMatchConfig(uint64_t sessionId) {
			NetMatchConfig config;
			config.sessionId = sessionId;
			config.hostPeerId = 1;
			config.players = {{1, 0, false, "Host"}, {2, 1, false, "Player"}};
			return config;
		}

		// A7 correction 1: the plane's clock is only right if the COMPOSITION is. NetLobbySession adds the
		// session clock it captured at Start to whatever Tick is handed, so handing it the session clock
		// counts it twice and every deadline inflates by the setup wait - and again by the whole previous
		// session at each rematch or resync. Measured here through a real lobby round, not the plane alone.
		int TestComposedSeatHoldMeetsP2() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint16_t port = 42150;
			LoopbackTransport hostTransport;
			LoopbackTransport holderTransport;
			NetSession host;
			NetSession holder;
			NetSeatAuthRegistry registry;
			registry.BeginHostedSession();
			NetReconnectHost admission;
			admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
			admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			NetReconnectTicketStore store;
			store.SetPath(StorePath("composed"));
			NetReconnectClient holderClient;
			uint64_t unixNow = 1'700'000'000'000ULL;
			holderClient.Configure(&store, MakeIdentity(), "Player");
			holderClient.SetUnixClock(&FixedUnixClock, &unixNow);
			holderClient.SetHostContext("loopback", MakeHash(5));
			host.SetReconnectHost(&admission);
			holder.SetReconnectClient(&holderClient);
			NetSessionConfig hostConfig = MakeSessionConfig(port, 101, "Host");
			hostConfig.maxPeers = 1;
			if (!host.StartHost(hostTransport, hostConfig, &error) ||
			    !holder.StartClient(holderTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not seat the holder: " + error);
			}

			// The service clock is session-elapsed; the setup wait costs 30 s, as an ordinary menu host does.
			uint64_t serviceMs = 0;
			auto step = [&](uint64_t byMs) {
				serviceMs += byMs;
				hostTransport.AdvanceTimeMs(byMs);
				holderTransport.AdvanceTimeMs(byMs);
			};
			for (uint64_t settled = 0; settled <= 600; settled += 10) {
				host.Tick(serviceMs);
				holder.Tick(serviceMs);
				step(10);
			}
			if (holderClient.GetState() != NetH4ClientState::Joined) {
				return Fail("the holder never committed its seat before the lobby round");
			}
			const uint64_t waitedMs = 30'000;
			for (uint64_t waited = 0; waited < waitedMs; waited += 100) {
				host.Tick(serviceMs);
				holder.Tick(serviceMs);
				step(100);
			}

			// The lobby round, clocked exactly as RunLobby clocks it.
			NetLobbySession lobby;
			NetLobbySessionConfig lobbyConfig;
			lobbyConfig.host = true;
			lobbyConfig.localPeerId = 1;
			lobbyConfig.remotePeerId = 2;
			lobbyConfig.remoteTransportPeerId = static_cast<NetPeerId>(1);
			lobbyConfig.matchConfig = MakeLobbyMatchConfig(0x5000000000000000ULL + port);
			lobbyConfig.timeoutMs = 600'000;
			lobbyConfig.session = &host;
			lobbyConfig.autoReady = true;
			lobbyConfig.autoStart = false;
			if (!lobby.Start(hostTransport, lobbyConfig, &error)) {
				return Fail("the lobby round would not start: " + error);
			}
			const uint64_t lobbyStartedAtMs = serviceMs;
			for (uint64_t roundMs = 0; roundMs <= 500; roundMs += 10) {
				const NetMatchRunnerClocks clocks = NetMatchRunner::ResolveRoundClocks(roundMs, true, serviceMs);
				lobby.Tick(clocks.lobbyMs);
				host.TickAdmissionPlane(clocks.planeMs);
				holder.Tick(serviceMs);
				step(10);
			}
			// A round that adds the session clock to itself shows up here, before any deadline; the hold
			// below is what it costs.
			const uint64_t inflationMs = host.GetClockMs() > serviceMs ? host.GetClockMs() - serviceMs : 0;
			if (host.GetReadyPeerCount() != 1) {
				return Fail("the lobby round dropped the seated player");
			}
			(void)lobbyStartedAtMs;

			// Live match: the holder drops, and the hold is measured in elapsed service milliseconds.
			admission.SetLiveMatch(true);
			holderTransport.Stop();
			const uint64_t droppedAtMs = serviceMs;
			uint64_t releasedAtMs = 0;
			for (uint64_t elapsed = 0; elapsed <= NetReconnectHost::c_ProvisionalExpiryMs * 3 && releasedAtMs == 0; elapsed += 10) {
				for (const NetTransportEvent& event: hostTransport.PollEvents()) {
					host.InjectEvent(event, serviceMs);
				}
				host.TickAdmissionPlane(serviceMs);
				if (!admission.IsSeatHeldForReclaim(MakeSeatTable()[0].lockstepPeerId)) {
					releasedAtMs = serviceMs;
				}
				step(10);
			}
			if (releasedAtMs == 0) {
				return Fail("the dropped seat was never released");
			}
			const uint64_t heldMs = releasedAtMs - droppedAtMs;
			if (heldMs < NetReconnectHost::c_ProvisionalExpiryMs || heldMs > NetReconnectHost::c_ProvisionalExpiryMs + 20) {
				return Fail("the composed P2 seat hold was " + std::to_string(heldMs) + " ms against the pinned " +
				            std::to_string(NetReconnectHost::c_ProvisionalExpiryMs) + ", the lobby round having inflated the session clock by " +
				            std::to_string(inflationMs) + " ms");
			}
			if (inflationMs != 0) {
				return Fail("the lobby round inflated the session clock by " + std::to_string(inflationMs) + " ms");
			}

			// What the round hands each part, taken from the runner itself rather than restated.
			const NetMatchRunnerClocks first = NetMatchRunner::ResolveRoundClocks(500, true, 30500);
			if (first.lobbyMs != 500) {
				return Fail("the lobby round was handed the session clock, which it adds its own base to");
			}
			if (first.planeMs != 30500) {
				return Fail("the admission plane was not handed session-elapsed time");
			}
			if (first.budgetMs != 500) {
				return Fail("the round's wait budget stopped being per round");
			}
			const NetMatchRunnerClocks standalone = NetMatchRunner::ResolveRoundClocks(500, false, 0);
			if (standalone.lobbyMs != 500 || standalone.planeMs != 500 || standalone.budgetMs != 500) {
				return Fail("a runner with no service clock stopped clocking itself");
			}
			return 0;
		}

		// A7 correction 4: P14 is "no decodable ClientHello within the budget". A connection that
		// heartbeats and never says hello is not authenticating, so its age is what expires it - reading
		// its last packet instead let eight of them hold the whole bound for as long as they kept talking.
		int TestHeartbeatingHandshakeStillExpires() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint16_t port = 42151;
			LoopbackTransport hostTransport;
			LoopbackTransport holderTransport;
			NetSession host;
			NetSession holder;
			NetSeatAuthRegistry registry;
			registry.BeginHostedSession();
			NetReconnectHost admission;
			admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
			admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
			NetReconnectTicketStore store;
			store.SetPath(StorePath("heartbeat"));
			NetReconnectClient holderClient;
			uint64_t unixNow = 1'700'000'000'000ULL;
			holderClient.Configure(&store, MakeIdentity(), "Player");
			holderClient.SetUnixClock(&FixedUnixClock, &unixNow);
			holderClient.SetHostContext("loopback", MakeHash(5));
			host.SetReconnectHost(&admission);
			holder.SetReconnectClient(&holderClient);
			NetSessionConfig hostConfig = MakeSessionConfig(port, 101, "Host");
			hostConfig.maxPeers = 1;
			if (!host.StartHost(hostTransport, hostConfig, &error) ||
			    !holder.StartClient(holderTransport, "loopback", MakeSessionConfig(port, 202, "Player"), &error)) {
				return Fail("could not seat the holder: " + error);
			}
			uint64_t nowMs = 0;
			for (; nowMs <= 600; nowMs += 10) {
				host.Tick(nowMs);
				holder.Tick(nowMs);
				hostTransport.AdvanceTimeMs(10);
				holderTransport.AdvanceTimeMs(10);
			}
			if (holderClient.GetState() != NetH4ClientState::Joined || host.GetReadyPeerCount() != 1) {
				return Fail("the holder never committed the only seat");
			}
			admission.SetLiveMatch(true);

			// Eight connections that talk and never say hello.
			std::vector<std::unique_ptr<LoopbackTransport>> chatty;
			for (uint32_t index = 0; index < NetSession::c_MaxUnauthenticatedPeers; ++index) {
				auto transport = std::make_unique<LoopbackTransport>();
				if (!transport->Connect("loopback", port, &error)) {
					return Fail("a heartbeating connection could not be made: " + error);
				}
				chatty.push_back(std::move(transport));
			}
			const uint64_t deadlineMs = MakeSessionConfig(port, 0, "Host").timeoutMs;
			uint64_t nextHeartbeatMs = nowMs;
			const uint64_t startedAtMs = nowMs;
			for (const uint64_t until = nowMs + deadlineMs * 6; nowMs <= until; nowMs += 10) {
				if (nowMs >= nextHeartbeatMs) {
					// Every 4 s, comfortably inside a 5 s budget measured from the last packet.
					nextHeartbeatMs = nowMs + 4000;
					for (const std::unique_ptr<LoopbackTransport>& transport: chatty) {
						NetMessage message;
						message.sequence = 1;
						message.payload = NetHeartbeat{};
						std::vector<uint8_t> bytes;
						if (NetProtocol::Encode(message, bytes)) {
							// A loopback client always addresses its host as peer 1.
							(void)transport->Send(static_cast<NetPeerId>(1), NetTransportLane::ControlReliable, bytes, nullptr);
						}
					}
				}
				for (const NetTransportEvent& event: hostTransport.PollEvents()) {
					host.InjectEvent(event, nowMs);
				}
				host.TickAdmissionPlane(nowMs);
				for (const std::unique_ptr<LoopbackTransport>& transport: chatty) {
					transport->AdvanceTimeMs(10);
				}
				hostTransport.AdvanceTimeMs(10);
				holderTransport.AdvanceTimeMs(10);
				if (host.GetUnauthenticatedPeerCount() == 0) {
					break;
				}
			}
			if (host.GetUnauthenticatedPeerCount() != 0) {
				return Fail("heartbeating connections that never said hello held the P14 bound: " +
				            std::to_string(host.GetUnauthenticatedPeerCount()) + " of " +
				            std::to_string(NetSession::c_MaxUnauthenticatedPeers));
			}
			const uint64_t expiredAfterMs = nowMs - startedAtMs;
			if (expiredAfterMs < deadlineMs || expiredAfterMs > deadlineMs * 2) {
				return Fail("a heartbeating handshake expired " + std::to_string(expiredAfterMs) +
				            " ms after connecting, against P14's " + std::to_string(deadlineMs));
			}
			// The controls the reviewer confirmed must survive it: the seated player stays, and a client
			// that is genuinely mid-handshake is not cut short before the deadline.
			if (host.GetReadyPeerCount() != 1) {
				return Fail("expiring heartbeating handshakes dropped the seated player");
			}
			return 0;
		}

		// Source40 R1 and R2 are one defect: a timeout must measure a silence the session was WATCHING.
		// The round owns the transport for the whole match, so the session is handed no traffic while it
		// plays and - since the plane's clock became real elapsed time - a clock that runs on anyway. The
		// first evaluation after that phase is the resync round's lobby tick, or the leave exchange's, and
		// it measures the entire match: the host evicts every peer before the resync round can form, and
		// the leaving client evicts the host before its LeaveRequest is flushed, so the announced leave is
		// adjudicated as a lost connection. Both arms are driven exactly as the service drives them.
		int TestARoundResumptionKeepsItsPeers() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			static_assert(NetReconnectClient::c_LeaveAckBudgetMs == 2000, "P21's ack budget");
			// Long enough that the match alone outlasts the session's timeout, which is the whole point.
			const uint64_t playedMs = 20'000;
			const uint32_t budgetMs = MakeSessionConfig(0, 0, "Host").timeoutMs;
			if (playedMs <= budgetMs) {
				return Fail("the match phase must outlast the session timeout for this case to say anything");
			}

			struct SeatedPair {
				LoopbackTransport hostTransport;
				LoopbackTransport clientTransport;
				NetSession host;
				NetSession client;
				NetSeatAuthRegistry registry;
				NetReconnectHost admission;
				NetReconnectTicketStore store;
				NetReconnectClient reconnect;
				uint64_t serviceMs = 0; //!< The admission clock: elapsed since the service started this match.

				void Step(uint64_t byMs) {
					serviceMs += byMs;
					hostTransport.AdvanceTimeMs(byMs);
					clientTransport.AdvanceTimeMs(byMs);
				}

				bool Seat(const std::string& name, uint16_t port, uint64_t* unixNow, std::string* error) {
					registry.BeginHostedSession();
					admission.Configure(&registry, 0x5000000000000000ULL + port, MakeIdentity());
					admission.SetSeatTable(MakeSeatTable(), NetMatchMode::PvPSkirmish);
					store.SetPath(StorePath(name));
					reconnect.Configure(&store, MakeIdentity(), "Player");
					reconnect.SetUnixClock(&FixedUnixClock, unixNow);
					reconnect.SetHostContext("loopback", MakeHash(5));
					host.SetReconnectHost(&admission);
					client.SetReconnectClient(&reconnect);
					if (!host.StartHost(hostTransport, MakeSessionConfig(port, 101, "Host"), error) ||
					    !client.StartClient(clientTransport, "loopback", MakeSessionConfig(port, 202, "Player"), error)) {
						return false;
					}
					// WaitForSessionReady, clocked from the service's elapsed time as the runner clocks it.
					for (uint64_t settled = 0; settled <= 600; settled += 10) {
						host.Tick(serviceMs);
						client.Tick(serviceMs);
						Step(10);
					}
					if (reconnect.GetState() != NetH4ClientState::Joined || host.GetReadyPeerCount() != 1) {
						*error = "the pair never committed its seat";
						return false;
					}
					admission.SetLiveMatch(true);
					return true;
				}

				// The match: PumpSessionEvents is all the session gets. The host's plane is ticked on the
				// service clock and forwarded events injected on it; the client's pump has nothing to do
				// and returns before touching its session at all.
				void PlayFor(uint64_t forMs) {
					for (const uint64_t until = serviceMs + forMs; serviceMs <= until;) {
						for (const NetTransportEvent& event: hostTransport.PollEvents()) {
							host.InjectEvent(event, serviceMs);
						}
						host.TickAdmissionPlane(serviceMs);
						Step(10);
					}
				}
			};

			uint64_t unixNow = 1'700'000'000'000ULL;

			// R1: the resync round. StartNextMatch re-runs the lobby over the same session, and the lobby
			// adds the session clock it captured at Start to its own round time - so the first tick lands
			// on a clock the match advanced while nothing could stamp a receive.
			{
				SeatedPair pair;
				if (!pair.Seat("resume-resync", 42152, &unixNow, &error)) {
					return Fail("R1 arm: " + error);
				}
				const uint64_t seatedAtMs = pair.serviceMs;
				pair.PlayFor(playedMs);
				if (pair.host.GetClockMs() - seatedAtMs <= budgetMs) {
					return Fail("R1 arm: the match never advanced the session clock past the timeout, so this arm is vacuous");
				}

				NetLobbySession hostLobby;
				NetLobbySession clientLobby;
				NetLobbySessionConfig hostLobbyConfig;
				hostLobbyConfig.host = true;
				hostLobbyConfig.localPeerId = 1;
				hostLobbyConfig.remotePeerId = 2;
				hostLobbyConfig.remoteTransportPeerId = static_cast<NetPeerId>(1);
				hostLobbyConfig.matchConfig = MakeLobbyMatchConfig(0x5000000000000000ULL + 42152);
				hostLobbyConfig.timeoutMs = 60'000;
				hostLobbyConfig.session = &pair.host;
				hostLobbyConfig.autoReady = true;
				hostLobbyConfig.autoStart = true;
				NetLobbySessionConfig clientLobbyConfig = hostLobbyConfig;
				clientLobbyConfig.host = false;
				clientLobbyConfig.localPeerId = 2;
				clientLobbyConfig.remotePeerId = 1;
				clientLobbyConfig.session = &pair.client;
				if (!hostLobby.Start(pair.hostTransport, hostLobbyConfig, &error) ||
				    !clientLobby.Start(pair.clientTransport, clientLobbyConfig, &error)) {
					return Fail("R1 arm: the resync round would not start: " + error);
				}
				bool formed = false;
				for (uint64_t roundMs = 0; roundMs <= 4000 && !formed; roundMs += 10) {
					const NetMatchRunnerClocks clocks = NetMatchRunner::ResolveRoundClocks(roundMs, true, pair.serviceMs);
					hostLobby.Tick(clocks.lobbyMs);
					pair.host.TickAdmissionPlane(clocks.planeMs);
					clientLobby.Tick(clocks.lobbyMs);
					pair.client.TickAdmissionPlane(clocks.planeMs);
					formed = hostLobby.IsStarted() && clientLobby.IsStarted();
					pair.Step(10);
				}
				if (!formed) {
					return Fail("R1 arm: the resync round never formed after the match: host=" +
					            std::string(NetLobbySession::StateName(hostLobby.GetState())) + " (" + hostLobby.GetFailureReason() +
					            ") client=" + NetLobbySession::StateName(clientLobby.GetState()) + " (" + clientLobby.GetFailureReason() +
					            ") session timeouts host=" + std::to_string(pair.host.GetStats().timeouts) +
					            " client=" + std::to_string(pair.client.GetStats().timeouts));
				}
				if (pair.host.GetReadyPeerCount() != 1 || !pair.client.IsReady()) {
					return Fail("R1 arm: the round formed but the session dropped its peers");
				}
				if (pair.host.GetStats().timeouts != 0 || pair.client.GetStats().timeouts != 0) {
					return Fail("R1 arm: a silence nobody was listening through was counted as a timeout");
				}
				// Exactly one resumption per match-to-round transition, and it is what let the round form.
				if (pair.host.GetStats().timeoutResumptions != 1) {
					return Fail("R1 arm: the round formed on " + std::to_string(pair.host.GetStats().timeoutResumptions) +
					            " host resumptions, not the one the transition owes");
				}

				// The control: a resumption starts the window again, it does not remove it, and it does
				// not move it either - the peer that goes quiet is evicted one budget later, measured.
				const uint64_t quietFromMs = pair.serviceMs;
				uint64_t evictedAfterMs = 0;
				while (pair.serviceMs - quietFromMs <= budgetMs * 3ULL && evictedAfterMs == 0) {
					pair.host.Tick(pair.serviceMs);
					if (pair.host.GetReadyPeerCount() == 0) {
						evictedAfterMs = pair.serviceMs - quietFromMs;
					}
					pair.Step(10);
				}
				if (evictedAfterMs == 0 || pair.host.GetStats().timeouts != 1) {
					return Fail("R1 arm: a peer that went quiet under a ticking session was never evicted, so this case proves nothing");
				}
				// The tolerance is one heartbeat interval either side: the peer's last stamped receive is
				// its last heartbeat, which lands somewhere inside the interval before the round ended.
				if (evictedAfterMs + 200 < budgetMs || evictedAfterMs > budgetMs + 200) {
					return Fail("R1 arm: the quiet peer was evicted after " + std::to_string(evictedAfterMs) +
					            " ms, not the budget's " + std::to_string(budgetMs));
				}
				if (pair.host.GetStats().timeoutResumptions != 1) {
					return Fail("R1 arm: the eviction window was restarted again while the session was watching");
				}
			}

			// F2.1c: the predicate has a floor. A caller that always evaluates more slowly than the
			// budget would otherwise resume forever and never evict anyone - twenty consecutive slow
			// evaluations kept a never-speaking peer seated for 100 s in the reviewer's probe. A
			// resumption is worth one restart per silence: once the window has been restarted, the next
			// full budget without a word ends the peer however slowly the caller evaluates from there.
			{
				SeatedPair pair;
				if (!pair.Seat("resume-floor", 42154, &unixNow, &error)) {
					return Fail("floor arm: " + error);
				}
				// The client is never ticked again: a peer that died without a FIN.
				pair.PlayFor(playedMs);
				uint32_t slowTicks = 0;
				while (slowTicks < 20 && pair.host.GetReadyPeerCount() != 0) {
					pair.host.Tick(pair.serviceMs);
					++slowTicks;
					pair.Step(budgetMs + 10);
				}
				if (pair.host.GetReadyPeerCount() != 0 || pair.host.GetStats().timeouts != 1) {
					return Fail("floor arm: " + std::to_string(slowTicks) + " evaluations at " +
					            std::to_string(budgetMs + 10) + " ms kept a never-speaking peer seated for " +
					            std::to_string(static_cast<uint64_t>(slowTicks) * (budgetMs + 10)) + " ms: resumptions=" +
					            std::to_string(pair.host.GetStats().timeoutResumptions) + " timeouts=" +
					            std::to_string(pair.host.GetStats().timeouts));
				}
				// One restart, then judged: the second slow evaluation is the one that evicts.
				if (pair.host.GetStats().timeoutResumptions != 1 || slowTicks != 2) {
					return Fail("floor arm: the peer went after " + std::to_string(slowTicks) + " evaluations and " +
					            std::to_string(pair.host.GetStats().timeoutResumptions) + " resumptions, not 2 and 1");
				}
			}

			// R2: the announced leave. RunCleanLeave opens the leave and then ticks the client's session on
			// the admission clock, which the match has carried far past the last thing the session heard.
			{
				SeatedPair pair;
				if (!pair.Seat("resume-leave", 42153, &unixNow, &error)) {
					return Fail("R2 arm: " + error);
				}
				pair.PlayFor(playedMs);
				if (!pair.reconnect.BeginLeave(pair.client.GetClockMs(), &error)) {
					return Fail("R2 arm: the leave would not start: " + error);
				}
				for (const uint64_t until = pair.serviceMs + NetReconnectClient::c_LeaveAckBudgetMs;
				     pair.serviceMs <= until && pair.reconnect.GetState() != NetH4ClientState::Left;) {
					pair.client.Tick(pair.serviceMs);
					// The host is still in its match, so its side of the exchange rides PumpSessionEvents.
					for (const NetTransportEvent& event: pair.hostTransport.PollEvents()) {
						pair.host.InjectEvent(event, pair.serviceMs);
					}
					pair.host.TickAdmissionPlane(pair.serviceMs);
					pair.Step(10);
				}
				if (pair.reconnect.GetState() != NetH4ClientState::Left) {
					return Fail(std::string("R2 arm: the announced leave was adjudicated as a lost connection: client=") +
					            NetReconnectClientStateName(pair.reconnect.GetState()) +
					            " acks=" + std::to_string(pair.reconnect.GetStats().leaveAcksReceived) +
					            " session=" + NetSession::StateName(pair.client.GetState()) +
					            " reject=" + pair.client.BuildRejectText());
				}
				if (pair.store.HasRecord() || pair.reconnect.GetStats().leaveAcksReceived != 1) {
					return Fail("R2 arm: an acknowledged leave did not clear the recovery record");
				}
				if (pair.admission.GetStats().seatsClosedByLeave != 1) {
					return Fail("R2 arm: the host did not close the seat on the leave");
				}
				if (pair.client.GetStats().timeouts != 0) {
					return Fail("R2 arm: the leave exchange timed the host out instead of talking to it");
				}
			}
			return 0;
		}

		// Source40 item 3: host_reseat_issued reads one log line, and IssueReseat has TWO ways of not
		// printing it - the drop ledgered nothing (a fault: the returner is reseated onto nothing), or the
		// ledger is good and none of the units it names is still alive (not a fault: there is nothing to
		// hand back). The gates could not tell them apart, and on reclaim_socket the host ended with
		// actors 2 of a peak 4 while drop3, which did print the line, ended with 5 of 6. Each answer gets
		// its own counter here, and the reseat itself is unchanged.
		//
		// F2.3: the counters alone still do not settle WHICH world a run is in - all the ledgered units
		// dead and a live unit the ledger never named read identically. The last arm is that second
		// world, and the number that tells them apart is the live-on-team count the record does not name.
		int TestAReseatSaysWhyItDidNotIssue() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint8_t leaverPeer = MakeSeatTable()[0].lockstepPeerId;
			// What the leaver held at the drop: three of its own units and one the host always had.
			const std::vector<NetH4LedgerActor> held = {
			    {101, 1, leaverPeer, true}, {102, 1, leaverPeer, true}, {103, 1, leaverPeer, true}, {201, 0, 1, true}};

			struct Arm {
				const char* label;
				NetPeerId connection;
				std::vector<NetH4LedgerActor> atTheDrop;
				std::vector<NetH4LedgerActor> atTheReclaim;
				uint32_t issued;
				uint32_t withoutALedger;
				uint32_t withoutSurvivors;
				uint32_t liveOnTeamNotNamed;
				std::vector<int64_t> reseated;
			};
			const std::vector<Arm> arms = {
			    // The good case, so the two counters below are read against a working reseat.
			    {"survivors", 141, held, held, 1, 0, 0, 0, {101, 102, 103}},
			    // reclaim_socket's shape: the ledger is right, the units did not live through the window.
			    {"no-survivors", 142, held, {{201, 0, 1, true}}, 0, 0, 1, 0, {}},
			    // A6's fault, kept measurable: the drop was seen where the world cannot be walked.
			    {"no-ledger", 143, {}, held, 0, 1, 0, 3, {}},
			    // F2.3's second world: same three counters as no-survivors, and a unit alive on the
			    // returner's team that its record never named. Only this number tells them apart.
			    {"unnamed-survivor", 144, held, {{104, 1, leaverPeer, true}, {201, 0, 1, true}}, 0, 0, 1, 1, {}},
			};

			uint64_t unixNow = 1'700'000'000'000ULL;
			for (const Arm& arm: arms) {
				const std::string where = std::string(" (") + arm.label + " arm)";
				std::vector<NetH4LedgerActor> census = arm.atTheDrop;
				Wire wire;
				ConfigureWire(wire);
				wire.host.SetDropOwnershipSource(
				    [](void* context) { return *static_cast<std::vector<NetH4LedgerActor>*>(context); }, &census);
				Endpoint player;
				player.connection = arm.connection;
				ConfigureEndpoint(player, std::string("reseat-") + arm.label, &unixNow);
				wire.Add(&player);
				NetH4TicketRecord record;
				if (SeatAndDrop(wire, player, record, unixNow, &error) != 0) {
					return Fail("could not seat the player: " + error + where);
				}
				const NetH4SeatOwnership* ledgered = wire.host.GetLedger().Find(0);
				if (ledgered == nullptr) {
					return Fail("the drop recorded no seat at all" + where);
				}
				if (ledgered->actorUIDs.size() != (arm.atTheDrop.empty() ? 0U : 3U)) {
					return Fail("the drop ledgered " + std::to_string(ledgered->actorUIDs.size()) + " units" + where);
				}

				// Whatever is left of the world when the holder comes back.
				census = arm.atTheReclaim;
				wire.host.SetLiveMatch(true);
				Endpoint returner;
				returner.connection = static_cast<NetPeerId>(arm.connection + 100);
				ConfigureEndpoint(returner, std::string("reseat-") + arm.label, &unixNow);
				wire.Add(&returner);
				wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
				if (!returner.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
					return Fail("the reclaim did not settle: " + error + where);
				}
				if (returner.client.GetState() != NetH4ClientState::Joined) {
					return Fail(std::string("the returner did not commit: ") + NetReconnectClientStateName(returner.client.GetState()) + where);
				}
				const std::vector<NetGameReseat> reseats = wire.host.TakePendingReseats();
				std::vector<int64_t> reseated;
				for (const NetGameReseat& reseat: reseats) {
					reseated.insert(reseated.end(), reseat.actorUIDs.begin(), reseat.actorUIDs.end());
				}
				if (reseated != arm.reseated) {
					return Fail("the reclaim reseated " + std::to_string(reseated.size()) + " units" + where);
				}
				const NetReconnectHostStats stats = wire.host.GetStats();
				if (stats.reseatsIssued != arm.issued || stats.reseatsWithoutALedger != arm.withoutALedger ||
				    stats.reseatsWithoutSurvivors != arm.withoutSurvivors ||
				    stats.reseatLiveOnTeamNotNamed != arm.liveOnTeamNotNamed) {
					return Fail("the reseat counters read issued=" + std::to_string(stats.reseatsIssued) +
					            " without_a_ledger=" + std::to_string(stats.reseatsWithoutALedger) +
					            " without_survivors=" + std::to_string(stats.reseatsWithoutSurvivors) +
					            " live_on_team_not_named=" + std::to_string(stats.reseatLiveOnTeamNotNamed) + where);
				}
			}
			// The two worlds the counters cannot separate, side by side: identical on all three, and
			// apart on the one number that says whether the record named the world it came back to.
			return 0;
		}

		// followup-5 item 4: a seat drops twice. The first drop is seen from the sim tick and ledgers what
		// the holder had; the second is seen from a setup worker - WaitForSessionReady ticking the session
		// on the rematch or resync round, then CheckTimeouts, DropPeerTransport, NotifyDisconnect - where
		// CollectDropOwnership refuses to walk the world and returns nothing. Replacing the record with
		// that empty one starves IssueReseat, so the next returner is reseated onto nothing.
		int TestASecondDropDoesNotEraseAGoodRecord() {
			ScriptedAuthCrypto crypto;
			ScopedTestCrypto scope(&crypto);
			std::string error;
			if (!ResetLaneDirectory(&error)) {
				return Fail(error);
			}
			const uint8_t leaverPeer = MakeSeatTable()[0].lockstepPeerId;
			const std::vector<int64_t> held = {101, 102, 103};

			// The rule on its own, where both drops are visible side by side.
			{
				NetReconnectLedger ledger;
				ledger.RecordDrop(0, leaverPeer, 1, 100, held);
				ledger.RecordDrop(0, leaverPeer, 1, 240, {});
				const NetH4SeatOwnership* record = ledger.Find(0);
				if (record == nullptr || record->actorUIDs != held || ledger.GetEmptyDropsRefused() != 1) {
					return Fail("an empty second drop erased the seat's recorded units");
				}
				// A later drop that HAS units is the seat's own news and still replaces it.
				ledger.RecordDrop(0, leaverPeer, 1, 300, {104});
				if (ledger.Find(0)->actorUIDs != std::vector<int64_t>{104} || ledger.GetEmptyDropsRefused() != 1) {
					return Fail("a later drop with units stopped replacing the seat's record");
				}
			}

			// And through the host, in the order production takes it.
			std::vector<NetH4LedgerActor> census = {
			    {101, 1, leaverPeer, true}, {102, 1, leaverPeer, true}, {103, 1, leaverPeer, true}, {201, 0, 1, true}};
			uint64_t unixNow = 1'700'000'000'000ULL;
			Wire wire;
			ConfigureWire(wire);
			wire.host.SetDropOwnershipSource(
			    [](void* context) { return *static_cast<std::vector<NetH4LedgerActor>*>(context); }, &census);
			Endpoint player;
			player.connection = 151;
			ConfigureEndpoint(player, "second-drop", &unixNow);
			wire.Add(&player);
			NetH4TicketRecord record;
			if (SeatAndDrop(wire, player, record, unixNow, &error) != 0) {
				return Fail("could not seat the player: " + error);
			}
			wire.host.SetLiveMatch(true);

			Endpoint first;
			first.connection = 152;
			ConfigureEndpoint(first, "second-drop", &unixNow);
			wire.Add(&first);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!first.client.BeginReclaim(record, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the first reclaim did not settle: " + error);
			}
			if (wire.host.TakePendingReseats().size() != 1) {
				return Fail("the first reclaim did not issue the ledgered reseat");
			}
			NetH4TicketRecord second;
			if (first.store.Load(unixNow, second, &error) != NetH4TicketLoadResult::Loaded) {
				return Fail("the reclaim left no record to come back with: " + error);
			}

			// The off-tick second drop: the census refuses, so the seat records nothing.
			const std::vector<NetH4LedgerActor> live = census;
			census.clear();
			wire.host.NotifyDisconnect(first.connection, 240);
			first.connected = false;
			wire.Remove(first.connection);
			census = live;
			const NetH4SeatOwnership* ledgered = wire.host.GetLedger().Find(0);
			if (ledgered == nullptr || ledgered->actorUIDs != held) {
				return Fail("the off-tick second drop erased the units the holder actually had");
			}
			if (wire.host.GetLedger().GetEmptyDropsRefused() != 1) {
				return Fail("the refusal was not counted, so a gate cannot see it");
			}

			// What the record is for: the next returner is reseated onto it.
			Endpoint returner;
			returner.connection = 153;
			ConfigureEndpoint(returner, "second-drop", &unixNow);
			wire.Add(&returner);
			wire.nowMs += NetReconnectAdmission::c_AttemptIntervalMs;
			if (!returner.client.BeginReclaim(second, wire.nowMs, &error) || !wire.Pump(&error)) {
				return Fail("the second reclaim did not settle: " + error);
			}
			const std::vector<NetGameReseat> reseats = wire.host.TakePendingReseats();
			if (reseats.size() != 1 || reseats.front().actorUIDs != held ||
			    reseats.front().newOwnerPeerId != leaverPeer) {
				return Fail("the returner after an off-tick second drop was reseated onto nothing");
			}
			if (wire.host.GetStats().reseatsWithoutALedger != 0) {
				return Fail("the second reclaim found the ledger empty");
			}
			return 0;
		}

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
		if (const int result = TestLedgerRecordsWhatTheLeaverHeld(); result != 0) {
			return result;
		}
		if (const int result = TestRejoinAcrossRematch(); result != 0) {
			return result;
		}
		if (const int result = TestSessionWiring(); result != 0) {
			return result;
		}
		if (const int result = TestReapedLobbySeatReturnsToThePool(); result != 0) {
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
		if (const int result = TestReclaimLadderFitsHandshakeTimeout(); result != 0) {
			return result;
		}
		if (const int result = TestReconnectUxSchedule(); result != 0) {
			return result;
		}
		if (const int result = TestSeatHoldWindow(); result != 0) {
			return result;
		}
		if (const int result = TestLeaveExchangeBeatsTeardown(); result != 0) {
			return result;
		}
		if (const int result = TestReturningHolderOnAFullSession(); result != 0) {
			return result;
		}
		if (const int result = TestLobbySeatIsFreedForTheNextJoiner(); result != 0) {
			return result;
		}
		if (const int result = TestRecoveryAppliesOnlyAfterAMatch(); result != 0) {
			return result;
		}
		if (const int result = TestLiveAdmissionExpiresSilentConnections(); result != 0) {
			return result;
		}
		if (const int result = TestAdmissionClockIsElapsedTime(); result != 0) {
			return result;
		}
		if (const int result = TestComposedSeatHoldMeetsP2(); result != 0) {
			return result;
		}
		if (const int result = TestHeartbeatingHandshakeStillExpires(); result != 0) {
			return result;
		}
		if (const int result = TestARoundResumptionKeepsItsPeers(); result != 0) {
			return result;
		}
		if (const int result = TestAReseatSaysWhyItDidNotIssue(); result != 0) {
			return result;
		}
		if (const int result = TestASecondDropDoesNotEraseAGoodRecord(); result != 0) {
			return result;
		}
		if (const int result = TestApplicantsAndBounds(); result != 0) {
			return result;
		}
		if (const int result = TestSubstitutionTransaction(); result != 0) {
			return result;
		}
		if (const int result = TestSubstitutionAckLost(); result != 0) {
			return result;
		}
		if (const int result = TestSubstitutionCleanup(); result != 0) {
			return result;
		}
		if (const int result = TestSubstitutionRaceBothOrders(); result != 0) {
			return result;
		}
		if (const int result = TestSubstituteOwnershipAndModeration(); result != 0) {
			return result;
		}
		std::cout << "[net-reconnect-session-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
