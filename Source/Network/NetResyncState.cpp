#include "NetResyncState.h"

#include "NetLockstep.h"
#include "NetLobbyProtocol.h"
#include "lz4.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace RTE {
	namespace {
		constexpr uint64_t c_Magic = 0x32534E595345524EULL;
		static_assert(NetResyncCodec::c_MaxTotalBytes <= NetLobbyProtocol::c_MaxTotalStateBytes);

		void Require(bool valid, const char* reason) {
			if (!valid) throw std::runtime_error(std::string("invalid resync envelope: ") + reason);
		}

		void Put(std::vector<uint8_t>& bytes, uint64_t value, size_t count) {
			Require(bytes.size() + count <= NetResyncCodec::c_MaxMetadataBytes, "metadata overflow");
			for (size_t i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
		}

		struct EnvelopeReader {
			const std::vector<uint8_t>& bytes;
			size_t cursor = 0, end;
			uint64_t Get(size_t count) {
				Require(cursor <= end && count <= end - cursor, "read past the metadata end");
				uint64_t value = 0;
				for (size_t i = 0; i < count; ++i) value |= static_cast<uint64_t>(bytes[cursor++]) << (i * 8);
				return value;
			}
		};

		bool ValidPeer(uint64_t peer) { return peer > 0 && peer <= NetLockstepCodec::c_MaxPeerCount; }
		bool Future(uint64_t frame, uint64_t savedTick) { return frame > savedTick && frame - savedTick - 1 <= NetLockstepCodec::c_MaxFutureFrameSkew; }

		void PutOwners(std::vector<uint8_t>& bytes, const std::map<int64_t, uint8_t>& owners) {
			Require(owners.size() <= NetResyncCodec::c_MaxAuxiliaryBytes / 9, "too many control owners");
			Put(bytes, owners.size(), 4);
			for (const auto& [uid, peer]: owners) {
				Require(uid > 0 && uid <= std::numeric_limits<int32_t>::max() && ValidPeer(peer), "control owner uid or peer out of range");
				Put(bytes, uid, 8); Put(bytes, peer, 1);
			}
		}

		void GetOwners(EnvelopeReader& reader, std::map<int64_t, uint8_t>& owners) {
			const auto count = reader.Get(4);
			Require(count <= (reader.end - reader.cursor) / 9 && count <= NetResyncCodec::c_MaxAuxiliaryBytes / 9, "control owner count past the metadata end");
			int64_t previous = 0;
			for (uint64_t i = 0; i < count; ++i) {
				const auto uid = reader.Get(8), peer = reader.Get(1);
				Require(uid > static_cast<uint64_t>(previous) && uid <= std::numeric_limits<int32_t>::max() && ValidPeer(peer), "control owners out of order or out of range");
				previous = static_cast<int64_t>(uid);
				owners.emplace(previous, static_cast<uint8_t>(peer));
			}
		}

		void PutCommand(std::vector<uint8_t>& bytes, uint64_t round, uint64_t frame, const NetGameCommand& command) {
			Require(ValidPeer(command.senderPeerId), "command sender peer out of range");
			NetLockstepFrame packet;
			packet.senderPeerId = command.senderPeerId;
			packet.roundId = round; packet.targetFrame = frame; packet.commands.push_back(command);
			std::vector<uint8_t> encoded;
			Require(NetLockstepCodec::Encode({packet}, encoded), "command did not encode");
			Put(bytes, encoded.size(), 4);
			Require(encoded.size() <= NetResyncCodec::c_MaxMetadataBytes - bytes.size(), "command overflows the metadata");
			bytes.insert(bytes.end(), encoded.begin(), encoded.end());
		}

		NetResyncPendingCommand GetCommand(EnvelopeReader& reader, uint64_t round) {
			const auto size = reader.Get(4);
			Require(size <= reader.end - reader.cursor, "command size past the metadata end");
			const auto decoded = NetLockstepCodec::Decode(reader.bytes.data() + reader.cursor, static_cast<size_t>(size));
			reader.cursor += static_cast<size_t>(size);
			Require(decoded.ok, "command did not decode");
			const auto* frame = std::get_if<NetLockstepFrame>(&decoded.packet.payload);
			Require(frame && frame->roundId == round && frame->frames.empty() && frame->observations.empty() && frame->commands.size() == 1, "command packet is not a single command of this round");
			return {frame->targetFrame, frame->commands.front()};
		}

		bool SameCommand(const NetGameCommand& left, const NetGameCommand& right, uint64_t round, uint64_t frame) {
			std::vector<uint8_t> first, second;
			Require(NetLockstepCodec::EncodeRecoveryInput({left.senderPeerId, frame, {}, {left}, round}, first), "command did not re-encode");
			Require(NetLockstepCodec::EncodeRecoveryInput({right.senderPeerId, frame, {}, {right}, round}, second), "command did not re-encode");
			return first == second;
		}

		void PutInput(std::vector<uint8_t>& bytes, const NetLockstepFrame& input) {
			std::vector<uint8_t> raw;
			Require(NetLockstepCodec::EncodeRecoveryInput(input, raw), "pending input did not encode");
			std::vector<uint8_t> packed(LZ4_compressBound(static_cast<int>(raw.size())));
			const int size = LZ4_compress_default(reinterpret_cast<const char*>(raw.data()), reinterpret_cast<char*>(packed.data()), static_cast<int>(raw.size()), static_cast<int>(packed.size()));
			const bool compressed = size > 0 && static_cast<size_t>(size) < raw.size();
			if (compressed) packed.resize(static_cast<size_t>(size)); else packed = std::move(raw);
			Put(bytes, compressed ? raw.size() : packed.size(), 4); Put(bytes, packed.size(), 4); Put(bytes, compressed, 1);
			Require(packed.size() <= NetResyncCodec::c_MaxMetadataBytes - bytes.size(), "pending input overflows the metadata");
			bytes.insert(bytes.end(), packed.begin(), packed.end());
		}

		NetLockstepFrame GetInput(EnvelopeReader& reader) {
			const auto rawSize = reader.Get(4), packedSize = reader.Get(4), compressed = reader.Get(1);
			Require(rawSize > 0 && rawSize <= NetLockstepCodec::c_MaxRecoveryInputBytes && packedSize > 0 && packedSize <= reader.end - reader.cursor, "pending input size out of range");
			Require((compressed == 0 && packedSize == rawSize) || (compressed == 1 && packedSize < rawSize), "pending input compression flag disagrees with its size");
			std::vector<uint8_t> raw(static_cast<size_t>(rawSize));
			if (compressed) {
				Require(LZ4_decompress_safe(reinterpret_cast<const char*>(reader.bytes.data() + reader.cursor), reinterpret_cast<char*>(raw.data()), static_cast<int>(packedSize), static_cast<int>(rawSize)) == rawSize, "pending input did not decompress");
			} else std::copy_n(reader.bytes.begin() + static_cast<std::ptrdiff_t>(reader.cursor), static_cast<size_t>(rawSize), raw.begin());
			reader.cursor += static_cast<size_t>(packedSize);
			NetLockstepFrame input;
			Require(NetLockstepCodec::DecodeRecoveryInput(raw, input), "pending input did not decode");
			return input;
		}

		using InputIndex = std::map<std::pair<uint8_t, uint64_t>, size_t>;
		void PutPending(std::vector<uint8_t>& bytes, const NetResyncPendingCommand& pending, const NetResyncState& state, const InputIndex& inputs, size_t& referenceBytes) {
			if (const auto found = inputs.find({pending.command.senderPeerId, pending.frame}); found != inputs.end()) {
				const auto& commands = state.pendingInputs[found->second].commands;
				for (size_t index = 0; index < commands.size(); ++index) {
					if (commands[index].sequence == pending.command.sequence && SameCommand(commands[index], pending.command, state.sourceRound, pending.frame)) {
						Put(bytes, 1, 1); Put(bytes, found->second, 4); Put(bytes, index, 2);
						referenceBytes += 7;
						Require(referenceBytes <= NetResyncCodec::c_MaxReferenceBytes, "too many command references");
						return;
					}
				}
			}
			Put(bytes, 0, 1); PutCommand(bytes, state.sourceRound, pending.frame, pending.command);
		}

		NetResyncPendingCommand GetPending(EnvelopeReader& reader, const NetResyncState& state, size_t& referenceBytes) {
			const auto kind = reader.Get(1);
			if (kind == 0) return GetCommand(reader, state.sourceRound);
			Require(kind == 1, "unknown pending command kind");
			const auto input = reader.Get(4), command = reader.Get(2);
			Require(input < state.pendingInputs.size() && command < state.pendingInputs[input].commands.size(), "command reference is out of range");
			referenceBytes += 7;
			Require(referenceBytes <= NetResyncCodec::c_MaxReferenceBytes, "too many command references");
			return {state.pendingInputs[input].targetFrame, state.pendingInputs[input].commands[command]};
		}
	}

	bool NetResyncCodec::Encode(const NetResyncState& state, const std::vector<uint8_t>& archive, std::vector<uint8_t>& bytes, std::string* error) {
		try {
			Require(state.sessionId != 0 && state.sourceRound != 0 && state.savedTick != UINT64_MAX && !archive.empty() && archive.size() <= c_MaxArchiveBytes, "session, round, savedTick or archive is unset");
			std::vector<uint8_t> result;
			Put(result, c_Magic, 8); Put(result, 0, 4); Put(result, archive.size(), 4);
			Put(result, state.sessionId, 8); Put(result, state.sourceRound, 8); Put(result, state.savedTick, 8);
			PutOwners(result, state.controlOwners); PutOwners(result, state.droppedControlOwners);
			Require(state.playerBindings.size() <= NetLockstepCodec::c_MaxPeerCount && state.appliedCommands.size() <= NetLockstepCodec::c_MaxPeerCount, "too many playerBindings or appliedCommands");
			Put(result, state.playerBindings.size(), 1);
			for (const auto& [peer, binding]: state.playerBindings) {
				Require(binding.frame <= state.savedTick, "playerBindings frame past savedTick");
				PutCommand(result, state.sourceRound, binding.frame, {peer, binding.bindings});
			}
			Put(result, state.appliedCommands.size(), 1);
			for (const auto& [peer, sequence]: state.appliedCommands) {
				Require(ValidPeer(peer) && sequence != UINT64_MAX, "appliedCommands peer or sequence out of range");
				Put(result, peer, 1); Put(result, sequence, 8);
			}
			Require(result.size() <= c_MaxAuxiliaryBytes && state.pendingInputs.size() <= c_MaxPendingInputs, "auxiliary metadata overflow or too many pendingInputs");
			Put(result, state.pendingInputs.size(), 4);
			const size_t inputStart = result.size();
			InputIndex inputTargets;
			for (size_t index = 0; index < state.pendingInputs.size(); ++index) {
				const auto& input = state.pendingInputs[index];
				Require(input.roundId == state.sourceRound, "pendingInputs round is not the source round");
				Require(Future(input.targetFrame, state.savedTick), "pendingInputs frame is not inside the window past savedTick");
				Require(inputTargets.emplace(std::make_pair(input.senderPeerId, input.targetFrame), index).second, "duplicate pendingInputs peer and frame");
				PutInput(result, input);
			}
			const size_t inputBytes = result.size() - inputStart;
			size_t referenceBytes = 0;
			const auto checkAuxiliary = [&] { Require(result.size() - inputBytes - referenceBytes <= c_MaxAuxiliaryBytes, "auxiliary metadata overflow"); };
			Put(result, state.pendingCommands.size(), 4);
			std::map<std::pair<uint8_t, uint64_t>, uint64_t> pendingTargets;
			for (const auto& pending: state.pendingCommands) {
				Require(Future(pending.frame, state.savedTick), "pendingCommands frame is not inside the window past savedTick");
				Require(pending.command.sequence != 0 && !std::holds_alternative<NetGamePlayerBindings>(pending.command.payload), "pendingCommands entry is a binding or has no sequence");
				const auto applied = state.appliedCommands.find(pending.command.senderPeerId);
				Require(applied == state.appliedCommands.end() || pending.command.sequence > applied->second, "pendingCommands sequence is already applied");
				Require(pendingTargets.emplace(std::make_pair(pending.command.senderPeerId, pending.command.sequence), pending.frame).second, "duplicate pendingCommands peer and sequence");
				PutPending(result, pending, state, inputTargets, referenceBytes);
				checkAuxiliary();
			}
			std::pair<uint8_t, uint64_t> previous{};
			for (const auto& [key, frame]: pendingTargets) {
				Require(previous.first != key.first || frame >= previous.second, "pendingCommands frames regress with the sequence");
				previous = {key.first, frame};
			}
			Put(result, state.pendingPlayerBindings.size(), 4);
			std::set<std::pair<uint8_t, uint64_t>> pendingBindings;
			for (const auto& pending: state.pendingPlayerBindings) {
				Require(Future(pending.frame, state.savedTick), "pendingPlayerBindings frame is not inside the window past savedTick");
				Require(pending.command.sequence == 0 && std::holds_alternative<NetGamePlayerBindings>(pending.command.payload), "pendingPlayerBindings entry is not a sequenceless binding");
				Require(pendingBindings.emplace(pending.command.senderPeerId, pending.frame).second, "duplicate pendingPlayerBindings peer and frame");
				PutPending(result, pending, state, inputTargets, referenceBytes);
				checkAuxiliary();
			}
			Put(result, state.admittedReseats.size(), 4);
			for (const auto& command: state.admittedReseats) {
				Require(command.sequence == 0 && std::holds_alternative<NetGameReseat>(command.payload), "admittedReseats entry is not a sequenceless reseat");
				PutCommand(result, state.sourceRound, state.savedTick, command);
				checkAuxiliary();
			}
			checkAuxiliary();
			Require(archive.size() <= c_MaxTotalBytes - result.size(), "archive overflows the envelope");
			const auto metadataSize = static_cast<uint32_t>(result.size());
			for (size_t i = 0; i < 4; ++i) result[8 + i] = static_cast<uint8_t>(metadataSize >> (i * 8));
			result.insert(result.end(), archive.begin(), archive.end());
			bytes = std::move(result);
			return true;
		} catch (const std::exception& exception) {
			if (error) *error = exception.what();
			return false;
		}
	}

	bool NetResyncCodec::Decode(const std::vector<uint8_t>& bytes, uint64_t sessionId, uint64_t startFrame, NetResyncState& state, std::vector<uint8_t>& archive, std::string* error) {
		try {
			Require(bytes.size() <= c_MaxTotalBytes, "envelope is too large");
			EnvelopeReader reader{bytes, 0, bytes.size()};
			Require(reader.Get(8) == c_Magic, "envelope magic is wrong");
			const auto metadataSize = reader.Get(4), archiveSize = reader.Get(4);
			Require(metadataSize >= reader.cursor && metadataSize <= c_MaxMetadataBytes && metadataSize <= bytes.size() && archiveSize > 0 && archiveSize <= c_MaxArchiveBytes && archiveSize == bytes.size() - metadataSize, "metadata or archive size is out of range");
			reader.end = static_cast<size_t>(metadataSize);
			NetResyncState result;
			result.sessionId = reader.Get(8); result.sourceRound = reader.Get(8); result.savedTick = reader.Get(8);
			Require(result.sessionId != 0 && result.sessionId == sessionId && result.sourceRound != 0 && result.savedTick != UINT64_MAX && result.savedTick + 1 == startFrame, "session, round or savedTick does not match the round");
			GetOwners(reader, result.controlOwners); GetOwners(reader, result.droppedControlOwners);
			const auto bindingCount = reader.Get(1);
			Require(bindingCount <= NetLockstepCodec::c_MaxPeerCount, "too many playerBindings");
			uint8_t previousPeer = 0;
			for (uint64_t i = 0; i < bindingCount; ++i) {
				const auto pending = GetCommand(reader, result.sourceRound);
				const auto* bindings = std::get_if<NetGamePlayerBindings>(&pending.command.payload);
				Require(bindings && pending.command.sequence == 0 && pending.frame <= result.savedTick && pending.command.senderPeerId > previousPeer, "playerBindings entry is out of order or past savedTick");
				previousPeer = pending.command.senderPeerId;
				result.playerBindings.emplace(previousPeer, NetResyncPlayerBindings{pending.frame, *bindings});
			}
			const auto appliedCount = reader.Get(1);
			Require(appliedCount <= NetLockstepCodec::c_MaxPeerCount, "too many appliedCommands");
			previousPeer = 0;
			for (uint64_t i = 0; i < appliedCount; ++i) {
				const auto peer = reader.Get(1), sequence = reader.Get(8);
				Require(ValidPeer(peer) && peer > previousPeer && sequence != UINT64_MAX, "appliedCommands are out of order or out of range");
				previousPeer = static_cast<uint8_t>(peer); result.appliedCommands.emplace(previousPeer, sequence);
			}
			Require(reader.cursor <= c_MaxAuxiliaryBytes, "auxiliary metadata overflow");
			const auto inputCount = reader.Get(4);
			Require(inputCount <= c_MaxPendingInputs && inputCount <= (reader.end - reader.cursor) / 9, "too many pendingInputs");
			const size_t inputStart = reader.cursor;
			std::set<std::pair<uint8_t, uint64_t>> inputTargets;
			for (uint64_t i = 0; i < inputCount; ++i) {
				auto input = GetInput(reader);
				Require(input.roundId == result.sourceRound && Future(input.targetFrame, result.savedTick), "pendingInputs round or frame does not fit the round");
				Require(inputTargets.emplace(input.senderPeerId, input.targetFrame).second, "duplicate pendingInputs peer and frame");
				result.pendingInputs.push_back(std::move(input));
			}
			const size_t inputBytes = reader.cursor - inputStart;
			size_t referenceBytes = 0;
			const auto checkAuxiliary = [&] { Require(reader.cursor - inputBytes - referenceBytes <= c_MaxAuxiliaryBytes, "auxiliary metadata overflow"); };
			const auto pendingCount = reader.Get(4);
			Require(pendingCount <= (reader.end - reader.cursor) / 4, "pendingCommands count past the metadata end");
			std::map<std::pair<uint8_t, uint64_t>, NetResyncPendingCommand> unique;
			for (uint64_t i = 0; i < pendingCount; ++i) {
				auto pending = GetPending(reader, result, referenceBytes);
				checkAuxiliary();
				const auto applied = result.appliedCommands.find(pending.command.senderPeerId);
				Require(Future(pending.frame, result.savedTick) && pending.command.sequence != 0 && (applied == result.appliedCommands.end() || pending.command.sequence > applied->second) && !std::holds_alternative<NetGamePlayerBindings>(pending.command.payload), "pendingCommands entry is outside the window, applied, a binding or has no sequence");
				Require(unique.emplace(std::make_pair(pending.command.senderPeerId, pending.command.sequence), pending).second, "duplicate pendingCommands peer and sequence");
				result.pendingCommands.push_back(std::move(pending));
			}
			std::pair<uint8_t, uint64_t> previous{};
			for (const auto& [key, pending]: unique) {
				Require(previous.first != key.first || pending.frame >= previous.second, "pendingCommands frames regress with the sequence");
				previous = {key.first, pending.frame};
			}
			const auto pendingBindingCount = reader.Get(4);
			Require(pendingBindingCount <= (reader.end - reader.cursor) / 4, "pendingPlayerBindings count past the metadata end");
			std::set<std::pair<uint8_t, uint64_t>> pendingBindings;
			for (uint64_t i = 0; i < pendingBindingCount; ++i) {
				auto pending = GetPending(reader, result, referenceBytes);
				checkAuxiliary();
				Require(Future(pending.frame, result.savedTick) && pending.command.sequence == 0 && std::holds_alternative<NetGamePlayerBindings>(pending.command.payload), "pendingPlayerBindings entry is outside the window or not a sequenceless binding");
				Require(pendingBindings.emplace(pending.command.senderPeerId, pending.frame).second, "duplicate pendingPlayerBindings peer and frame");
				result.pendingPlayerBindings.push_back(std::move(pending));
			}
			const auto reseatCount = reader.Get(4);
			Require(reseatCount <= (reader.end - reader.cursor) / 4, "admittedReseats count past the metadata end");
			for (uint64_t i = 0; i < reseatCount; ++i) {
				auto pending = GetCommand(reader, result.sourceRound);
				checkAuxiliary();
				Require(pending.frame == result.savedTick && pending.command.sequence == 0 && std::holds_alternative<NetGameReseat>(pending.command.payload), "admittedReseats entry is not a sequenceless reseat at savedTick");
				result.admittedReseats.push_back(std::move(pending.command));
			}
			checkAuxiliary();
			Require(reader.cursor == reader.end, "metadata has trailing bytes");
			std::vector<uint8_t> decodedArchive(bytes.begin() + static_cast<std::ptrdiff_t>(metadataSize), bytes.end());
			state = std::move(result); archive = std::move(decodedArchive);
			return true;
		} catch (const std::exception& exception) {
			if (error) *error = exception.what();
			return false;
		}
	}
}
