#include "NetResyncSelfTest.h"

#include "NetLockstep.h"
#include "NetResyncState.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RTE {

	namespace {
		using Bytes = std::vector<uint8_t>;
		constexpr uint64_t c_SequenceEnd = std::numeric_limits<uint64_t>::max();
		const Bytes c_Archive{0x43, 0x43, 0x53, 0x41, 0x56, 0x45, 0x00, 0xFF, 0x7E};

		void Check(bool value, const std::string& message) {
			if (!value) throw std::runtime_error(message);
		}

		Bytes Hex(std::string_view text) {
			Check(text.size() % 2 == 0, "odd fixture hex length");
			const auto digit = [](char c) {
				if (c >= '0' && c <= '9') return c - '0';
				Check(c >= 'a' && c <= 'f', "invalid fixture hex digit");
				return c - 'a' + 10;
			};
			Bytes bytes;
			for (size_t i = 0; i < text.size(); i += 2) bytes.push_back(static_cast<uint8_t>((digit(text[i]) << 4) | digit(text[i + 1])));
			return bytes;
		}

		void WriteLE(Bytes& bytes, size_t offset, uint64_t value, size_t count) {
			Check(offset <= bytes.size() && count <= bytes.size() - offset, "fixture write outside packet");
			for (size_t i = 0; i < count; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
		}

		void FixPacketLength(Bytes& bytes) {
			Check(bytes.size() >= 16, "fixture has no lockstep header");
			WriteLE(bytes, 12, bytes.size() - 16, 4);
		}

		Bytes SplicePacket(Bytes bytes, size_t offset, size_t count, const Bytes& replacement) {
			Check(offset <= bytes.size() && count <= bytes.size() - offset, "fixture splice outside packet");
			bytes.erase(bytes.begin() + offset, bytes.begin() + offset + count);
			bytes.insert(bytes.begin() + offset, replacement.begin(), replacement.end());
			FixPacketLength(bytes);
			return bytes;
		}

		Bytes EncodePacket(const NetLockstepPacket& packet) {
			Bytes bytes;
			NetLockstepError error;
			const bool ok = NetLockstepCodec::Encode(packet, bytes, &error);
			Check(ok, "valid packet encode refused: " + error.message);
			return bytes;
		}

		template <typename T>
		T DecodePacket(const Bytes& bytes) {
			const auto decoded = NetLockstepCodec::Decode(bytes);
			Check(decoded.ok, "valid packet decode refused: " + decoded.error.message);
			const auto* payload = std::get_if<T>(&decoded.packet.payload);
			Check(payload != nullptr, "decoded packet type changed");
			return *payload;
		}

		void RejectPacket(const Bytes& bytes, const std::string& label) {
			Check(!NetLockstepCodec::Decode(bytes).ok, label + " decoded successfully");
		}

		void RejectPacketEncode(const NetLockstepPacket& packet, const std::string& label) {
			const Bytes before{0x81, 0xA5, 0x7F};
			Bytes output = before;
			Check(!NetLockstepCodec::Encode(packet, output), label + " encoded successfully");
			Check(output == before, label + " changed the encode destination");
		}

		void PacketTruncations(const Bytes& bytes) {
			for (size_t length = 0; length < bytes.size(); ++length) {
				Bytes prefix(bytes.begin(), bytes.begin() + length);
				const auto label = "packet truncation at " + std::to_string(length);
				RejectPacket(prefix, label);
				if (length >= 16) {
					FixPacketLength(prefix);
					RejectPacket(prefix, label + " with matching outer length");
				}
			}
		}

		NetLockstepFrame Frame(std::vector<NetGameCommand> commands) {
			NetLockstepFrame frame;
			frame.senderPeerId = 2;
			frame.targetFrame = 0x0102030405060708ULL;
			frame.roundId = 0x8877665544332211ULL;
			frame.commands = std::move(commands);
			return frame;
		}

		NetGamePlayerBindings Bindings() {
			NetGamePlayerBindings bindings;
			for (size_t i = 0; i < bindings.players.size(); ++i) {
				auto& player = bindings.players[i];
				player.active = player.human = true;
				player.hadBrain = i != 2;
				player.brainEvacuated = i == 3;
				player.team = static_cast<int8_t>(i);
				player.viewState = static_cast<uint8_t>(i * 3);
				player.controlledUID = 101 + static_cast<int64_t>(i);
				player.brainUID = i == 1 ? 0 : 9001 + static_cast<int64_t>(i);
				player.cameraX = std::bit_cast<float>(0x3F800001U + static_cast<uint32_t>(i));
				player.cameraY = std::bit_cast<float>(0x80000000U);
				const std::array<uint32_t, 8> targets{0, 0x80000000U, 0x3F800001U, 0xC0800001U, 0x7F7FFFFFU, 0x00800000U, 1, 0x3E800000U};
				for (size_t j = 0; j < targets.size(); ++j) player.viewTargets[j] = std::bit_cast<float>(targets[j]);
			}
			bindings.players[2].team = -1;
			bindings.players[2].controlledUID = bindings.players[2].brainUID = 0;
			bindings.players[3].controlledUID = bindings.players[3].brainUID = std::numeric_limits<int32_t>::max();
			bindings.appliedCommands = {{1, 0}, {2, 10}, {16, c_SequenceEnd - 1}};
			return bindings;
		}

		void SameBindings(const NetGamePlayerBindings& actual, const NetGamePlayerBindings& expected) {
			Check(actual == expected, "player binding fields changed");
			for (size_t i = 0; i < actual.players.size(); ++i) {
				const auto& a = actual.players[i];
				const auto& b = expected.players[i];
				const auto same = [](float x, float y) { return std::bit_cast<uint32_t>(x) == std::bit_cast<uint32_t>(y); };
				Check(same(a.cameraX, b.cameraX) && same(a.cameraY, b.cameraY), "camera float bits changed at slot " + std::to_string(i));
				for (size_t j = 0; j < a.viewTargets.size(); ++j) Check(same(a.viewTargets[j], b.viewTargets[j]), "view-target float bits changed");
			}
		}

		void TestBindingRoundTrips() {
			for (auto bindings : {Bindings(), NetGamePlayerBindings{}}) {
				const auto packet = Frame({{2, bindings}});
				const auto bytes = EncodePacket({packet});
				const auto decoded = DecodePacket<NetLockstepFrame>(bytes);
				Check(decoded == packet, "binding frame identity or commands changed");
				SameBindings(std::get<NetGamePlayerBindings>(decoded.commands.front().payload), bindings);
				Check(EncodePacket({decoded}) == bytes, "binding packet bytes changed after decode");
				PacketTruncations(bytes);
			}
			auto sparse = Bindings();
			sparse.players[1] = sparse.players[2] = {};
			const auto decoded = DecodePacket<NetLockstepFrame>(EncodePacket({Frame({{2, sparse}})}));
			SameBindings(std::get<NetGamePlayerBindings>(decoded.commands.front().payload), sparse);
		}

		void TestSignedZeroSlot() {
			for (size_t slot = 0; slot < 4; ++slot) {
				for (int field = 0; field < 10; ++field) {
					NetGamePlayerBindings bindings;
					auto& p = bindings.players[slot];
					float& value = field == 0 ? p.cameraX : field == 1 ? p.cameraY : p.viewTargets[field - 2];
					value = std::bit_cast<float>(0x80000000U);
					const auto decoded = DecodePacket<NetLockstepFrame>(EncodePacket({Frame({{2, bindings}})}));
					SameBindings(std::get<NetGamePlayerBindings>(decoded.commands.front().payload), bindings);
				}
			}
		}

		void TestCommandCapacityAndOrder() {
			std::vector<NetGameCommand> commands;
			for (uint64_t i = 1; i <= 256; ++i) commands.push_back({2, NetGameSetTeamFunds{static_cast<int32_t>(i % 4), static_cast<int32_t>(1000 - i)}, i});
			commands.insert(commands.begin() + 127, {2, Bindings()});
			const auto expected = Frame(commands);
			const auto encoded = EncodePacket({expected});
			const auto decoded = DecodePacket<NetLockstepFrame>(encoded);
			Check(decoded == expected && decoded.commands.size() == 257, "256 real commands plus one binding lost data or order");
			SameBindings(std::get<NetGamePlayerBindings>(decoded.commands[127].payload), Bindings());
			PacketTruncations(encoded);
			commands.push_back({2, NetGamePauseMatch{0, true}, 257});
			RejectPacketEncode({Frame(commands)}, "257 real commands plus a binding");
			commands.erase(commands.begin() + 127);
			RejectPacketEncode({Frame(commands)}, "257 real commands without a binding");
			RejectPacketEncode({Frame({{2, Bindings()}, {2, NetGamePlayerBindings{}}})}, "duplicate binding commands");

			const auto bytes = EncodePacket({Frame({{2, NetGameSetTeamFunds{0, 3}, 1}})});
			commands.pop_back();
			const auto extra = EncodePacket({Frame({{2, NetGameSetTeamFunds{0, 3}, 257}})});
			auto realOverflow = EncodePacket({Frame(commands)});
			realOverflow = SplicePacket(realOverflow, realOverflow.size() - 11, 0, Bytes(extra.begin() + 30, extra.end() - 11));
			WriteLE(realOverflow, 28, 257, 2);
			RejectPacket(realOverflow, "257 wire commands without a binding");
			auto overflow = bytes;
			WriteLE(overflow, 28, 258, 2);
			RejectPacket(overflow, "wire command count overflow");
			auto duplicate = EncodePacket({Frame({{2, NetGamePlayerBindings{}}})});
			const Bytes repeated(duplicate.begin() + 30, duplicate.begin() + 35);
			duplicate.insert(duplicate.begin() + 35, repeated.begin(), repeated.end());
			WriteLE(duplicate, 28, 2, 2);
			FixPacketLength(duplicate);
			RejectPacket(duplicate, "two wire binding commands");
		}

		void TestCommandSequences() {
			for (const uint64_t sequence : {uint64_t{0}, uint64_t{1}, uint64_t{127}, uint64_t{128}, uint64_t{1} << 63, c_SequenceEnd - 1}) {
				const auto expected = Frame({{2, NetGameSetTeamFunds{0, 777}, sequence}, {2, NetGamePauseMatch{0, false}, 9}});
				const auto bytes = EncodePacket({expected});
				Check(DecodePacket<NetLockstepFrame>(bytes) == expected, "command sequence or order changed");
				PacketTruncations(bytes);
			}
			RejectPacketEncode({Frame({{2, NetGameSetTeamFunds{0, 1}, c_SequenceEnd}})}, "UINT64_MAX command sequence");
			RejectPacketEncode({Frame({{2, Bindings(), 1}})}, "sequenced player observation");
			const auto bytes = EncodePacket({Frame({{2, NetGameSetTeamFunds{0, 1}, 1}})});
			RejectPacket(SplicePacket(bytes, 32, 1, Hex("ffffffffffffffffff01")), "wire UINT64_MAX command sequence");
			RejectPacket(SplicePacket(bytes, 32, 1, Hex("ffffffffffffffffff02")), "wire command varint overflow");
			auto observation = EncodePacket({Frame({{2, NetGamePlayerBindings{}}})});
			observation[32] = 1;
			RejectPacket(observation, "wire sequenced player observation");
		}

		void TestBindingLimits() {
			NetGamePlayerBindings valid;
			valid.players[0].active = true;
			valid.players[0].team = 0;
			valid.players[0].controlledUID = 1;
			valid.players[0].brainUID = 2;
			const auto bytes = EncodePacket({Frame({{2, valid}})});
			for (int field = 0; field < 10; ++field) {
				for (const uint32_t bits : {0x7F800000U, 0xFF800000U, 0x7FC12345U}) {
					auto bad = valid;
					auto& p = bad.players[0];
					float& value = field == 0 ? p.cameraX : field == 1 ? p.cameraY : p.viewTargets[field - 2];
					value = std::bit_cast<float>(bits);
					RejectPacketEncode({Frame({{2, bad}})}, "non-finite player float " + std::to_string(field));
					auto wire = bytes;
					WriteLE(wire, 39 + 4 * field, bits, 4);
					RejectPacket(wire, "wire non-finite player float " + std::to_string(field));
				}
			}
			for (const int team : {-2, 4}) {
				auto bad = valid;
				bad.players[0].team = static_cast<int8_t>(team);
				RejectPacketEncode({Frame({{2, bad}})}, "player team outside [-1,3]");
			}
			for (int field = 0; field < 5; ++field) {
				auto bad = valid;
				if (field == 0) bad.players[0].viewState = 10;
				if (field == 1) bad.players[0].controlledUID = -1;
				if (field == 2) bad.players[0].brainUID = -1;
				if (field == 3) bad.players[0].controlledUID = int64_t{std::numeric_limits<int32_t>::max()} + 1;
				if (field == 4) bad.players[0].brainUID = int64_t{std::numeric_limits<int32_t>::max()} + 1;
				RejectPacketEncode({Frame({{2, bad}})}, "invalid player view or UID " + std::to_string(field));
			}
			for (const auto [offset, value] : {std::pair<size_t, uint8_t>{33, 0x10}, {34, 0x10}, {35, 5}, {36, 10}}) {
				auto bad = bytes;
				bad[offset] = value;
				RejectPacket(bad, "wire player mask, flags, team or view");
			}
			for (const size_t offset : {size_t{37}, size_t{38}}) {
				RejectPacket(SplicePacket(bytes, offset, 1, Hex("8080808008")), "wire UID exceeds INT32_MAX");
				RejectPacket(SplicePacket(bytes, offset, 1, Hex("ffffffffffffffffff01")), "wire UID exceeds INT64_MAX");
			}
			auto redundant = bytes;
			std::fill(redundant.begin() + 34, redundant.begin() + 79, 0);
			RejectPacket(redundant, "explicit all-default slot");
		}

		NetLockstepChecksum Checksum() {
			NetLockstepChecksum checksum;
			checksum.senderPeerId = 1;
			checksum.frame = 0x0102030405060708ULL;
			checksum.roundId = 0x8877665544332211ULL;
			for (size_t i = 0; i < checksum.hash.size(); ++i) checksum.hash[i] = static_cast<uint8_t>(i * 17);
			return checksum;
		}

		void TestChecksumAcks() {
			auto expected = Checksum();
			for (uint8_t peer = 1; peer <= NetLockstepCodec::c_MaxPeerCount; ++peer) expected.appliedCommands[peer] = peer == 1 ? 0 : peer == 16 ? c_SequenceEnd - 1 : uint64_t{peer} << 32;
			const auto bytes = EncodePacket({expected});
			Check(DecodePacket<NetLockstepChecksum>(bytes) == expected, "checksum acknowledgement map changed");
			PacketTruncations(bytes);
			for (const uint8_t peer : {uint8_t{0}, uint8_t{NetLockstepCodec::c_MaxPeerCount + 1}}) {
				auto bad = Checksum();
				bad.appliedCommands[peer] = 1;
				RejectPacketEncode({bad}, "invalid acknowledgement peer");
			}
			auto bad = expected;
			bad.appliedCommands[17] = 1;
			RejectPacketEncode({bad}, "acknowledgement count overflow");
			bad = Checksum();
			bad.appliedCommands[1] = c_SequenceEnd;
			RejectPacketEncode({bad}, "UINT64_MAX acknowledgement");
			auto small = Checksum();
			small.appliedCommands = {{1, 1}, {2, 2}};
			const auto wire = EncodePacket({small});
			for (const auto [offset, value] : {std::pair<size_t, uint8_t>{68, 17}, {69, 0}, {69, 17}, {71, 1}}) {
				auto invalid = wire;
				invalid[offset] = value;
				RejectPacket(invalid, "wire acknowledgement count or peer");
			}
			auto reversed = wire;
			std::swap(reversed[69], reversed[71]);
			RejectPacket(reversed, "descending acknowledgement peers");
			RejectPacket(SplicePacket(wire, 70, 1, Hex("ffffffffffffffffff01")), "wire UINT64_MAX acknowledgement");
		}

		void TestBindingAcks() {
			NetGamePlayerBindings expected;
			for (uint8_t peer = 1; peer <= 16; ++peer) expected.appliedCommands[peer] = peer == 1 ? 0 : peer == 16 ? c_SequenceEnd - 1 : uint64_t{peer} << 32;
			const auto bytes = EncodePacket({Frame({{2, expected}})});
			const auto decoded = DecodePacket<NetLockstepFrame>(bytes);
			SameBindings(std::get<NetGamePlayerBindings>(decoded.commands.front().payload), expected);
			PacketTruncations(bytes);
			for (const uint8_t peer : {uint8_t{0}, uint8_t{17}}) {
				NetGamePlayerBindings bad;
				bad.appliedCommands[peer] = 1;
				RejectPacketEncode({Frame({{2, bad}})}, "invalid binding acknowledgement peer");
			}
			auto bad = expected;
			bad.appliedCommands[17] = 1;
			RejectPacketEncode({Frame({{2, bad}})}, "binding acknowledgement count overflow");
			bad = expected;
			bad.appliedCommands[1] = c_SequenceEnd;
			RejectPacketEncode({Frame({{2, bad}})}, "UINT64_MAX binding acknowledgement");
			NetGamePlayerBindings small;
			small.appliedCommands = {{1, 1}, {2, 2}};
			const auto wire = EncodePacket({Frame({{2, small}})});
			for (const auto [offset, value] : {std::pair<size_t, uint8_t>{34, 17}, {35, 0}, {35, 17}, {37, 1}}) {
				auto invalid = wire;
				invalid[offset] = value;
				RejectPacket(invalid, "wire binding acknowledgement count or peer");
			}
			auto reversed = wire;
			std::swap(reversed[35], reversed[37]);
			RejectPacket(reversed, "descending binding acknowledgement peers");
			RejectPacket(SplicePacket(wire, 36, 1, Hex("ffffffffffffffffff01")), "wire UINT64_MAX binding acknowledgement");
		}

		void TestLegacyBytes() {
			// Literal v8/v17 wire fixtures, independent of the current encoder.
			const std::array<Bytes, 2> frames{
				Hex("43434c3308001000020000001f000000" "0300000001020304050607080200" "01000100000004030201" "06000200000001"),
				Hex("43434c3311001000020000002a000000" "0300000001020304050607080200" "01000100000004030201" "06000200000001" "1122334455667788000000")};
			const std::array<Bytes, 2> checksums{
				Hex("43434c3308001000050000002c000000" "040000000102030405060708" "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"),
				Hex("43434c33110010000500000034000000" "040000000102030405060708" "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f" "1122334455667788")};
			for (size_t i = 0; i < frames.size(); ++i) {
				const auto frame = DecodePacket<NetLockstepFrame>(frames[i]);
				const uint64_t round = i == 0 ? 0 : 0x8877665544332211ULL;
				const std::vector<NetGameCommand> commands{{3, NetGameSetTeamFunds{1, 0x01020304}}, {3, NetGamePauseMatch{2, true}}};
				Check(frame.senderPeerId == 3 && frame.targetFrame == 0x0807060504030201ULL && frame.roundId == round && frame.frames.empty() && frame.observations.empty() && frame.commands == commands, "legacy frame shape or zero command sequence changed");
				const auto checksum = DecodePacket<NetLockstepChecksum>(checksums[i]);
				Check(checksum.senderPeerId == 4 && checksum.frame == 0x0807060504030201ULL && checksum.roundId == round && checksum.appliedCommands.empty(), "legacy checksum acquired new fields");
				for (size_t j = 0; j < checksum.hash.size(); ++j) Check(checksum.hash[j] == j, "legacy checksum hash changed");
				PacketTruncations(frames[i]);
				PacketTruncations(checksums[i]);
			}
		}

		NetResyncState State() {
			NetResyncState state;
			state.sessionId = 0x1122334455667788ULL;
			state.sourceRound = 0x8877665544332211ULL;
			state.savedTick = 100;
			state.controlOwners = {{101, 2}, {std::numeric_limits<int32_t>::max(), 16}};
			state.droppedControlOwners = {{301, 3}};
			state.playerBindings = {{2, {100, Bindings()}}, {3, {99, {}}}};
			state.appliedCommands = {{2, 10}, {16, c_SequenceEnd - 1}};
			state.pendingCommands = {{101, {2, NetGameSetTeamFunds{0, 3}, 11}}, {101, {3, NetGameSetTeamFunds{1, 5}, 11}}, {102, {2, NetGameSetTeamFunds{0, 7}, 12}}};
			state.pendingPlayerBindings = {{101, {2, Bindings()}}, {101, {3, NetGamePlayerBindings{}}}, {102, {2, NetGamePlayerBindings{}}}};
			state.admittedReseats = {{1, NetGameReseat{0, 2, {101, 102}}}, {1, NetGameReseat{3, 16, {999}}}};
			return state;
		}

		Bytes EncodeState(const NetResyncState& state) {
			Bytes bytes;
			std::string error;
			const bool ok = NetResyncCodec::Encode(state, c_Archive, bytes, &error);
			Check(ok, "valid envelope encode refused: " + error);
			return bytes;
		}

		void RejectStateEncode(const NetResyncState& state, const std::string& label, const Bytes& archive = c_Archive) {
			const Bytes before{0x13, 0x57, 0x9B, 0xDF};
			Bytes bytes = before;
			Check(!NetResyncCodec::Encode(state, archive, bytes), label + " encoded successfully");
			Check(bytes == before, label + " changed the envelope destination");
		}

		void RejectState(const Bytes& bytes, uint64_t session, uint64_t start, const std::string& label) {
			const auto before = State();
			auto output = before;
			const Bytes archiveBefore{0x39, 0, 0xA7};
			auto archive = archiveBefore;
			Check(!NetResyncCodec::Decode(bytes, session, start, output, archive), label + " decoded successfully");
			Check(output == before && archive == archiveBefore, label + " changed decode outputs");
			for (const auto& [peer, value] : before.playerBindings) SameBindings(output.playerBindings.at(peer).bindings, value.bindings);
			for (size_t i = 0; i < before.pendingPlayerBindings.size(); ++i) SameBindings(std::get<NetGamePlayerBindings>(output.pendingPlayerBindings[i].command.payload), std::get<NetGamePlayerBindings>(before.pendingPlayerBindings[i].command.payload));
		}

		void StateRoundTrip(const NetResyncState& expected) {
			const auto bytes = EncodeState(expected);
			NetResyncState decoded;
			Bytes archive;
			Check(NetResyncCodec::Decode(bytes, expected.sessionId, expected.savedTick + 1, decoded, archive), "valid envelope decode refused");
			Check(decoded == expected && archive == c_Archive, "envelope changed state, command order or archive bytes");
			for (const auto& [peer, value] : expected.playerBindings) SameBindings(decoded.playerBindings.at(peer).bindings, value.bindings);
			for (size_t i = 0; i < expected.pendingPlayerBindings.size(); ++i) SameBindings(std::get<NetGamePlayerBindings>(decoded.pendingPlayerBindings[i].command.payload), std::get<NetGamePlayerBindings>(expected.pendingPlayerBindings[i].command.payload));
			for (size_t i = 0; i < expected.pendingInputs.size(); ++i) {
				Bytes before, after;
				Check(NetLockstepCodec::EncodeRecoveryInput(expected.pendingInputs[i], before) && NetLockstepCodec::EncodeRecoveryInput(decoded.pendingInputs[i], after) && before == after,
				      "complete input bits or command order changed");
			}
			Check(EncodeState(decoded) == bytes, "envelope encoding changed after decode");
		}

		void TestEnvelopeRoundTrips() {
			StateRoundTrip(State());
			auto boundary = State();
			boundary.pendingCommands.back().frame = boundary.pendingPlayerBindings.back().frame = boundary.savedTick + 1 + NetLockstepCodec::c_MaxFutureFrameSkew;
			StateRoundTrip(boundary);
			NetResyncState empty;
			empty.sessionId = empty.sourceRound = 1;
			StateRoundTrip(empty);
			empty.sessionId = empty.sourceRound = c_SequenceEnd;
			empty.savedTick = c_SequenceEnd - 1;
			StateRoundTrip(empty);
		}

		void TestEnvelopeRefusalAtomicity() {
			const auto expected = State();
			const auto bytes = EncodeState(expected);
			for (size_t length = 0; length < bytes.size(); ++length) RejectState(Bytes(bytes.begin(), bytes.begin() + length), expected.sessionId, 101, "envelope truncation at " + std::to_string(length));
			const size_t metadataSize = bytes.size() - c_Archive.size();
			for (size_t length = 16; length < metadataSize; ++length) {
				Bytes prefix(bytes.begin(), bytes.begin() + length);
				prefix.insert(prefix.end(), c_Archive.begin(), c_Archive.end());
				WriteLE(prefix, 8, length, 4);
				RejectState(prefix, expected.sessionId, 101, "metadata truncation at " + std::to_string(length));
			}
			RejectState(bytes, expected.sessionId + 1, 101, "wrong session identity");
			RejectState(bytes, expected.sessionId, 100, "start at saved tick");
			RejectState(bytes, expected.sessionId, 102, "start after next tick");
			for (const auto [offset, value, count] : {std::array<uint64_t, 3>{0, 0, 8}, {8, NetResyncCodec::c_MaxMetadataBytes + 1, 4}, {12, 0, 4}, {24, 0, 8}, {24, expected.sourceRound + 1, 8}, {32, c_SequenceEnd, 8}, {40, 0xFFFFFFFFU, 4}}) {
				auto bad = bytes;
				WriteLE(bad, static_cast<size_t>(offset), value, static_cast<size_t>(count));
				RejectState(bad, expected.sessionId, 101, "invalid envelope field at " + std::to_string(offset));
			}
			auto trailing = bytes;
			trailing.push_back(0);
			RejectState(trailing, expected.sessionId, 101, "unadvertised trailing archive byte");
		}

		void TestEnvelopeInputLimits() {
			for (int field = 0; field < 10; ++field) {
				auto bad = State();
				if (field == 0) bad.sessionId = 0;
				if (field == 1) bad.sourceRound = 0;
				if (field == 2) bad.savedTick = c_SequenceEnd;
				if (field == 3) bad.playerBindings.at(2).frame = 101;
				if (field == 4) bad.appliedCommands[2] = c_SequenceEnd;
				if (field == 5) bad.pendingCommands.front().frame = 100;
				if (field == 6) bad.pendingCommands.front().command.sequence = 0;
				if (field == 7) bad.pendingCommands.front().command.sequence = c_SequenceEnd;
				if (field == 8) bad.admittedReseats.front().sequence = 1;
				if (field == 9) bad.pendingCommands.back().frame = bad.savedTick + NetLockstepCodec::c_MaxFutureFrameSkew + 2;
				RejectStateEncode(bad, "invalid envelope input " + std::to_string(field));
			}
			for (const int64_t uid : {int64_t{0}, int64_t{-1}, int64_t{std::numeric_limits<int32_t>::max()} + 1}) {
				for (const bool dropped : {false, true}) {
					auto bad = State();
					(dropped ? bad.droppedControlOwners : bad.controlOwners)[uid] = 2;
					RejectStateEncode(bad, "owner UID outside native range");
				}
			}
			for (const uint8_t peer : {uint8_t{0}, uint8_t{17}}) {
				for (int field = 0; field < 5; ++field) {
					auto bad = State();
					if (field == 0) bad.controlOwners[101] = peer;
					if (field == 1) bad.droppedControlOwners[301] = peer;
					if (field == 2) bad.playerBindings[peer] = {100, Bindings()};
					if (field == 3) bad.appliedCommands[peer] = 0;
					if (field == 4) bad.pendingCommands.front().command.senderPeerId = peer;
					RejectStateEncode(bad, "invalid envelope peer " + std::to_string(field));
				}
			}
			RejectStateEncode(State(), "empty archive", {});
			auto bad = State();
			bad.admittedReseats.front().payload = NetGameSetTeamFunds{0, 4};
			RejectStateEncode(bad, "admitted non-reseat command");
		}

		Bytes PendingPacket(const NetResyncState& state, const NetResyncPendingCommand& pending) {
			auto frame = Frame({pending.command});
			frame.senderPeerId = pending.command.senderPeerId;
			frame.targetFrame = pending.frame;
			frame.roundId = state.sourceRound;
			return EncodePacket({frame});
		}

		Bytes ReplacePacket(Bytes bytes, const Bytes& before, const Bytes& after) {
			Check(before.size() == after.size(), "embedded fixture packet sizes differ");
			const auto found = std::search(bytes.begin(), bytes.end(), before.begin(), before.end());
			Check(found != bytes.end(), "embedded fixture packet missing");
			Check(std::search(found + 1, bytes.end(), before.begin(), before.end()) == bytes.end(), "embedded fixture packet is ambiguous");
			std::copy(after.begin(), after.end(), found);
			return bytes;
		}

		void TestFutureCommandInputIds() {
			for (int kind = 0; kind < 4; ++kind) {
				auto bad = State();
				if (kind == 0) bad.pendingCommands.back() = bad.pendingCommands.front();
				if (kind == 1) bad.pendingCommands.back().command.sequence = bad.pendingCommands.front().command.sequence;
				if (kind == 2) bad.pendingCommands.front().command.sequence = bad.appliedCommands.at(2);
				if (kind == 3) bad.pendingCommands.front().frame = bad.pendingCommands.back().frame + 1;
				RejectStateEncode(bad, "duplicate, conflicting or applied future command ID " + std::to_string(kind));
			}
		}

		void TestFutureCommandWireIds() {
			const auto state = State();
			const auto bytes = EncodeState(state);
			for (int kind = 0; kind < 7; ++kind) {
				const auto& original = kind == 5 ? state.pendingCommands.front() : state.pendingCommands.back();
				auto pending = original;
				if (kind == 0) pending = state.pendingCommands.front();
				if (kind == 1) pending.command.sequence = state.pendingCommands.front().command.sequence;
				if (kind == 2) pending.command.sequence = state.appliedCommands.at(2);
				if (kind == 3) pending.frame = state.savedTick;
				if (kind == 5) pending.command.sequence = state.pendingCommands.back().command.sequence + 1;
				if (kind == 6) pending.frame = state.savedTick + NetLockstepCodec::c_MaxFutureFrameSkew + 2;
				auto changed = state;
				if (kind == 4) ++changed.sourceRound;
				RejectState(ReplacePacket(bytes, PendingPacket(state, original), PendingPacket(changed, pending)), state.sessionId, 101, "invalid embedded future command " + std::to_string(kind));
			}
		}

		void TestFuturePlayerBindings() {
			const auto state = State();
			const auto bytes = EncodeState(state);
			for (int kind = 0; kind < 5; ++kind) {
				auto bad = state;
				if (kind == 0) bad.pendingPlayerBindings.back().frame = bad.pendingPlayerBindings.front().frame;
				if (kind == 1) bad.pendingPlayerBindings.back().frame = state.savedTick;
				if (kind == 2) bad.pendingPlayerBindings.back().frame = state.savedTick + NetLockstepCodec::c_MaxFutureFrameSkew + 2;
				if (kind == 3) bad.pendingPlayerBindings.back().command = {2, NetGameSetTeamFunds{0, 1}, 12};
				if (kind == 4) bad.pendingPlayerBindings.back().command.senderPeerId = 0;
				RejectStateEncode(bad, "invalid future player binding " + std::to_string(kind));
				if (kind < 3) RejectState(ReplacePacket(bytes, PendingPacket(state, state.pendingPlayerBindings.back()), PendingPacket(state, bad.pendingPlayerBindings.back())), state.sessionId, 101, "wire invalid future player binding " + std::to_string(kind));
			}
			auto sequenced = state;
			sequenced.pendingPlayerBindings.back().command.sequence = 1;
			RejectStateEncode(sequenced, "sequenced future player binding");
		}

		NetResyncState InputState() {
			NetResyncState state;
			state.sessionId = 71; state.sourceRound = 97; state.savedTick = 100;
			NetLockstepFrame input;
			input.senderPeerId = 1; input.targetFrame = 101; input.roundId = state.sourceRound;
			input.commands = {{1, NetGameSetTeamFunds{0, 99}, 1}, {1, NetGamePlayerBindings{}}};
			state.pendingInputs.push_back(input);
			state.pendingCommands.push_back({input.targetFrame, input.commands[0]});
			state.pendingPlayerBindings.push_back({input.targetFrame, input.commands[1]});
			return state;
		}

		void TestCompleteInputWindow() {
			auto state = InputState();
			state.pendingInputs.clear(); state.pendingCommands.clear(); state.pendingPlayerBindings.clear();
			size_t rawTotal = 0;
			for (uint8_t peer: {uint8_t{1}, uint8_t{2}}) {
				for (uint64_t index = 0; index < NetLockstepCodec::c_MaxInputDelayFrames; ++index) {
					auto input = InputState().pendingInputs.front();
					input.senderPeerId = peer; input.targetFrame += index;
					input.commands = {{peer, NetGameSetTeamFunds{peer - 1, static_cast<int32_t>(index)}, index + 1}, {peer, Bindings()}};
					ControllerFrame controller;
					controller.actorUniqueID = 1000 + peer;
					controller.stateMask = uint64_t{1} << WEAPON_FIRE;
					controller.analogAimX = 2300; controller.analogAimY = -1700;
					controller.aimAngle = -0.0F;
					input.frames.push_back(controller);
					for (size_t observation = 0; observation < NetLockstepCodec::c_MaxObservationsPerPacket; ++observation) {
						input.observations.push_back({peer, 100000 + index * 5000 + observation, index + 1, observation + 1, observation * 3,
						                             observation + 1, observation == 0 ? -0.0F : static_cast<float>(observation % 127) / 128.0F});
					}
					Bytes raw;
					Check(NetLockstepCodec::EncodeRecoveryInput(input, raw), "full observation input did not encode");
					rawTotal += raw.size();
					state.pendingCommands.push_back({input.targetFrame, input.commands[0]});
					state.pendingPlayerBindings.push_back({input.targetFrame, input.commands[1]});
					state.pendingInputs.push_back(std::move(input));
				}
			}
			Check(rawTotal > 2 * NetResyncCodec::c_MaxAuxiliaryBytes, "large recovery fixture did not exceed the old cap");
			StateRoundTrip(state);
			Check(EncodeState(state).size() < rawTotal, "lossless input blocks did not reduce repetitive recovery state");
			state = InputState();
			state.pendingInputs[0].targetFrame = state.pendingCommands[0].frame = state.pendingPlayerBindings[0].frame = state.savedTick + 1 + NetLockstepCodec::c_MaxFutureFrameSkew;
			StateRoundTrip(state);
			++state.pendingInputs[0].targetFrame;
			RejectStateEncode(state, "full input outside inclusive pending window");
			state = InputState();
			state.pendingInputs.push_back(state.pendingInputs.front());
			RejectStateEncode(state, "duplicate full input target");
		}

		uint32_t Read32(const Bytes& bytes, size_t offset) {
			Check(offset + 4 <= bytes.size(), "fixture integer outside envelope");
			uint32_t value = 0;
			for (size_t index = 0; index < 4; ++index) value |= uint32_t{bytes[offset + index]} << (index * 8);
			return value;
		}

		void TestCompleteInputRefusals() {
			const auto state = InputState();
			const auto bytes = EncodeState(state);
			// Empty owner, player and acknowledgement tables place the input record at byte 54.
			constexpr size_t record = 54;
			Check(Read32(bytes, record - 4) == 1, "input fixture layout changed");
			for (const auto [offset, value, width]: {std::array<uint64_t, 3>{record - 4, NetResyncCodec::c_MaxPendingInputs + 1, 4},
			     {record, 0, 4}, {record, NetLockstepCodec::c_MaxRecoveryInputBytes + 1, 4}, {record + 4, 0, 4},
			     {record + 4, NetLockstepCodec::c_MaxRecoveryInputBytes + 1, 4}, {record + 8, 2, 1}}) {
				auto bad = bytes;
				WriteLE(bad, offset, value, width);
				RejectState(bad, state.sessionId, 101, "invalid complete input block header");
			}
			const size_t pending = record + 9 + Read32(bytes, record + 4) + 4;
			Check(bytes[pending] == 1, "pending command was not stored as an exact input reference");
			for (const auto [offset, value, width]: {std::array<uint64_t, 3>{pending, 2, 1}, {pending + 1, 1, 4}, {pending + 5, 2, 2}}) {
				auto bad = bytes;
				WriteLE(bad, offset, value, width);
				RejectState(bad, state.sessionId, 101, "invalid complete input command reference");
			}
			Bytes raw;
			Check(NetLockstepCodec::EncodeRecoveryInput(state.pendingInputs.front(), raw), "raw input fixture did not encode");
			Bytes uncompressed(bytes.begin(), bytes.begin() + record);
			const size_t oldEnd = record + 9 + Read32(bytes, record + 4);
			uncompressed.resize(record + 9);
			WriteLE(uncompressed, record, raw.size(), 4); WriteLE(uncompressed, record + 4, raw.size(), 4);
			uncompressed[record + 8] = 0;
			uncompressed.insert(uncompressed.end(), raw.begin(), raw.end());
			uncompressed.insert(uncompressed.end(), bytes.begin() + oldEnd, bytes.end());
			WriteLE(uncompressed, 8, uncompressed.size() - c_Archive.size(), 4);
			NetResyncState decoded;
			Bytes archive;
			Check(NetResyncCodec::Decode(uncompressed, state.sessionId, 101, decoded, archive) && decoded == state && archive == c_Archive, "raw input fallback changed state");
			uncompressed[record + 9] ^= 1;
			RejectState(uncompressed, state.sessionId, 101, "invalid raw complete input");
			if (bytes[record + 8] == 1) {
				auto bad = bytes;
				std::fill(bad.begin() + record + 9, bad.begin() + oldEnd, 0xFF);
				RejectState(bad, state.sessionId, 101, "invalid compressed complete input");
			}
		}
	}

	int NetResyncSelfTest::Run() {
		const std::pair<const char*, void (*)()> tests[]{
			{"player_bindings_roundtrip", TestBindingRoundTrips},
			{"player_binding_signed_zero", TestSignedZeroSlot},
			{"command_capacity_and_order", TestCommandCapacityAndOrder},
			{"command_sequences", TestCommandSequences},
			{"player_binding_limits", TestBindingLimits},
			{"checksum_applied_commands", TestChecksumAcks},
			{"player_binding_applied_commands", TestBindingAcks},
			{"legacy_v8_v17_bytes", TestLegacyBytes},
			{"resync_roundtrip", TestEnvelopeRoundTrips},
			{"resync_refusal_atomicity", TestEnvelopeRefusalAtomicity},
			{"resync_input_limits", TestEnvelopeInputLimits},
			{"future_command_input_ids", TestFutureCommandInputIds},
			{"future_command_wire_ids", TestFutureCommandWireIds},
			{"future_player_bindings", TestFuturePlayerBindings},
			{"complete_input_window", TestCompleteInputWindow},
			{"complete_input_refusals", TestCompleteInputRefusals}};
		int failures = 0;
		for (const auto& [name, test] : tests) {
			try {
				test();
				std::cout << "[net-resync-selftest] PASS " << name << std::endl;
			} catch (const std::exception& error) {
				++failures;
				std::cerr << "[net-resync-selftest] FAIL " << name << ": " << error.what() << std::endl;
			}
		}
		return failures == 0 ? 0 : 1;
	}

} // namespace RTE
