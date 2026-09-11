#include "NetRecoveryJournal.h"

#include "NetA7Journal.h"
#include "NetResyncState.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <string_view>

namespace RTE {

	namespace {
		using json = nlohmann::json;
		std::mutex s_Mutex;
		std::set<std::string> s_Blobs;
		std::map<std::string, uint64_t> s_Acceptances;
		uint64_t s_AcceptanceOrdinal = 0;
		size_t s_BlobBytes = 0;
		std::atomic<uint64_t> s_Scope{0};
		constexpr size_t c_ChunkBytes = 4096, c_StateBytes = 8U * 1024U * 1024U, c_TotalBytes = 16U * 1024U * 1024U;

		template <class Map> json Pairs(const Map& values) {
			json result = json::array();
			for (const auto& [key, value]: values) result.push_back({key, value});
			return result;
		}
	}

	nlohmann::json NetRecoveryJournal::Context(const NetLockstepConfig& config, uint64_t round) {
		return {{"epoch", NetA7Journal::Hex(config.seatPresenceEpoch.data(), config.seatPresenceEpoch.size())}, {"session_id", config.sessionId}, {"wire_round", round}};
	}

	std::string NetRecoveryJournal::Blob(const char* kind, const std::vector<uint8_t>& bytes) {
		if (!NetA7Journal::RecoveryInputJournalEnabled()) return {};
		const bool input = std::string_view(kind) == "input";
		if ((!input && std::string_view(kind) != "state_json") || bytes.empty() || bytes.size() > (input ? NetLockstepCodec::c_MaxRecoveryInputBytes : c_StateBytes)) {
			NetA7Journal::Gap("recovery blob exceeds its declared bound");
			return {};
		}
		const std::string hash = NetA7Journal::Sha256(bytes.data(), bytes.size());
		if (hash.empty()) { NetA7Journal::Gap("recovery blob hash unavailable"); return {}; }
		std::lock_guard<std::mutex> lock(s_Mutex);
		if (s_Blobs.contains(hash)) return hash;
		if (s_Blobs.size() >= 65536 || bytes.size() > c_TotalBytes - s_BlobBytes) { NetA7Journal::Gap("recovery journal payload budget exceeded"); return {}; }
		s_Blobs.insert(hash);
		s_BlobBytes += bytes.size();
		for (size_t offset = 0; offset < bytes.size(); offset += c_ChunkBytes) {
			const size_t count = std::min(c_ChunkBytes, bytes.size() - offset);
			NetA7Journal::Emit("recovery_blob", {{"phase", "chunk"}, {"kind", kind}, {"blob_sha256", hash}, {"total_bytes", bytes.size()}, {"offset", offset}, {"bytes_hex", NetA7Journal::Hex(bytes.data() + offset, count)}});
		}
		NetA7Journal::Emit("recovery_blob", {{"phase", "complete"}, {"kind", kind}, {"blob_sha256", hash}, {"total_bytes", bytes.size()}, {"chunk_count", (bytes.size() + c_ChunkBytes - 1) / c_ChunkBytes}});
		return hash;
	}

	std::string NetRecoveryJournal::NextScope() {
		return std::to_string(++s_Scope);
	}

	nlohmann::json NetRecoveryJournal::Command(const NetGameCommand& command, uint64_t target) {
		NetLockstepFrame frame;
		frame.senderPeerId = command.senderPeerId;
		frame.roundId = 1;
		frame.commands.push_back(command);
		std::vector<uint8_t> bytes;
		if (!NetLockstepCodec::EncodeRecoveryInput(frame, bytes)) { NetA7Journal::Gap("cannot encode observed game command"); return json::object(); }
		const auto hash = NetA7Journal::Sha256(bytes.data(), bytes.size());
		frame.commands.front().sequence = 0;
		std::vector<uint8_t> payload;
		if (!NetLockstepCodec::EncodeRecoveryInput(frame, payload)) { NetA7Journal::Gap("cannot encode observed command payload"); return json::object(); }
		return {{"sender_peer_id", command.senderPeerId}, {"sequence", command.sequence}, {"target_frame", target}, {"type", NetGameCommandTypeName(NetGameCommandTypeOf(command.payload))},
			{"hash_algorithm", "command_only_recovery_v1"}, {"command_sha256", hash}, {"payload_sha256", NetA7Journal::Sha256(payload.data(), payload.size())},
			{"blob_sha256", NetA7Journal::RecoveryInputJournalEnabled() ? Blob("input", bytes) : hash}};
	}

	nlohmann::json NetRecoveryJournal::Input(const NetLockstepConfig& config, const NetLockstepFrame& input, const char* phase, const char* source, const std::string& scope, size_t index) {
		if (!NetA7Journal::RecoveryInputJournalEnabled()) return json::object();
		std::vector<uint8_t> bytes;
		if (!NetLockstepCodec::EncodeRecoveryInput(input, bytes)) { NetA7Journal::Gap("cannot encode observed recovery input"); return json::object(); }
		const std::string hash = Blob("input", bytes);
		json fields = Context(config, input.roundId);
		fields.update({{"sender_peer_id", input.senderPeerId}, {"target_frame", input.targetFrame}});
		const std::string identity = fields.dump();
		const std::string receipt = NetA7Journal::Sha256(reinterpret_cast<const uint8_t*>(identity.data()), identity.size());
		uint64_t ordinal = 0;
		{
			std::lock_guard<std::mutex> lock(s_Mutex);
			if (s_Acceptances.size() >= 65536 && !s_Acceptances.contains(receipt)) { NetA7Journal::Gap("recovery acceptance budget exceeded"); return json::object(); }
			auto& accepted = s_Acceptances[receipt];
			if (accepted == 0 && (std::string_view(phase) == "accepted_local" || std::string_view(phase) == "accepted_remote")) accepted = ++s_AcceptanceOrdinal;
			ordinal = accepted;
		}
		fields.update({{"phase", phase}, {"source", source}, {"blob_sha256", hash}, {"byte_count", bytes.size()}, {"acceptance_id", receipt}, {"acceptance_ordinal", ordinal},
			{"controller_count", input.frames.size()}, {"command_count", input.commands.size()}, {"observation_count", input.observations.size()}, {"scope_id", scope}, {"index", index}});
		NetA7Journal::Emit("recovery_input", fields);
		return fields;
	}

	std::string NetRecoveryJournal::State(const NetLockstepConfig& config, const NetResyncState& state, const char* phase, const char* source, uint64_t round, const std::vector<NetLockstepFrame>& extras, const NetResyncState* captured) {
		if (!NetA7Journal::RecoveryInputJournalEnabled()) return {};
		std::vector<uint8_t> metadata;
		if (!NetResyncCodec::Encode(captured ? *captured : state, {0}, metadata)) { NetA7Journal::Gap("cannot encode observed recovery state"); return {}; }
		const std::string scope = NextScope();
		json values = {{"control_owners", Pairs(state.controlOwners)}, {"dropped_control_owners", Pairs(state.droppedControlOwners)}, {"applied_commands", Pairs(state.appliedCommands)},
			{"player_bindings", json::array()}, {"pending_commands", json::array()}, {"pending_player_bindings", json::array()}, {"admitted_reseats", json::array()}, {"pending_inputs", json::array()}, {"local_extras", json::array()}};
		for (const auto& [peer, bindings]: state.playerBindings) values["player_bindings"].push_back(Command(NetGameCommand{peer, bindings.bindings, 0}, bindings.frame));
		for (const auto& pending: state.pendingCommands) values["pending_commands"].push_back(Command(pending.command, pending.frame));
		for (const auto& pending: state.pendingPlayerBindings) values["pending_player_bindings"].push_back(Command(pending.command, pending.frame));
		for (const auto& command: state.admittedReseats) values["admitted_reseats"].push_back(Command(command, state.savedTick + 1));
		const char* inputPhase = std::string_view(phase) == "capture" ? "captured" : "installed_authoritative";
		for (size_t index = 0; index < state.pendingInputs.size(); ++index) values["pending_inputs"].push_back(Input(config, state.pendingInputs[index], inputPhase, source, scope, index));
		for (size_t index = 0; index < extras.size(); ++index) values["local_extras"].push_back(Input(config, extras[index], "installed_local_extra", source, scope, index));
		const std::string serialized = values.dump();
		const std::string stateHash = Blob("state_json", {serialized.begin(), serialized.end()});
		auto fields = Context(config, round);
		fields.update({{"phase", phase}, {"source", source}, {"scope_id", scope}, {"saved_tick", state.savedTick}, {"input_count", state.pendingInputs.size()}, {"local_extra_count", extras.size()},
			{"metadata_sha256", NetA7Journal::Sha256(metadata.data(), metadata.size())}, {"state_blob_sha256", stateHash}});
		NetA7Journal::Emit("recovery_boundary", fields);
		return scope;
	}

	std::vector<NetLockstepFrame> NetRecoveryJournal::SplitReady(const NetLockstepConfig& config, uint64_t round, const NetLockstepReadyFrame& ready) {
		std::vector<NetLockstepFrame> result;
		if (ready.hasLocalInput) {
			NetLockstepFrame input;
			input.senderPeerId = config.localPeerId; input.targetFrame = ready.frame; input.roundId = round;
			input.frames = ready.localFrames; input.commands = ready.localCommands; input.observations = ready.localObservations;
			result.push_back(std::move(input));
		}
		size_t offset = 0;
		for (const auto& [peer, count]: ready.remoteFrameCounts) {
			if (count > ready.remoteFrames.size() - std::min(offset, ready.remoteFrames.size())) { NetA7Journal::Gap("ready input has inconsistent controller membership"); return {}; }
			NetLockstepFrame input;
			input.senderPeerId = peer; input.targetFrame = ready.frame; input.roundId = round;
			input.frames.assign(ready.remoteFrames.begin() + offset, ready.remoteFrames.begin() + offset + count);
			for (const auto& command: ready.remoteCommands) if (command.senderPeerId == peer) input.commands.push_back(command);
			for (const auto& observation: ready.remoteObservations) if (observation.senderPeerId == peer) input.observations.push_back(observation);
			result.push_back(std::move(input));
			offset += count;
		}
		if (offset != ready.remoteFrames.size()) NetA7Journal::Gap("ready input has unclaimed controller frames");
		return result;
	}

}
