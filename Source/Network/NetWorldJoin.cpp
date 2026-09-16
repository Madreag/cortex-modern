#include "NetWorldJoin.h"

#include "NetIdentity.h"
#include "System.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace RTE {

	namespace {
		using json = nlohmann::json;

		constexpr const char* c_RecordTag = "NetWorld1";

		std::string HexByte(unsigned value) {
			static const char* digits = "0123456789abcdef";
			std::string out(2, '0');
			out[0] = digits[(value >> 4) & 0xFU];
			out[1] = digits[value & 0xFU];
			return out;
		}
	}

	const char* NetWorldJoinPhaseName(NetWorldJoinPhase phase) {
		switch (phase) {
			case NetWorldJoinPhase::Idle: return "idle";
			case NetWorldJoinPhase::Authenticating: return "authenticating";
			case NetWorldJoinPhase::SnapshotTransfer: return "snapshot-transfer";
			case NetWorldJoinPhase::CatchingUp: return "catching-up";
			case NetWorldJoinPhase::Spectating: return "spectating";
			case NetWorldJoinPhase::Active: return "active";
			case NetWorldJoinPhase::Failed: return "failed";
		}
		return "unknown";
	}

#pragma region World identity

	std::string NetWorldIdentityFile::MakeWorldId() {
		// A world id is an admission namespace, never a simulation value: it takes the platform's
		// entropy and leaves the deterministic session value alone.
		std::random_device device;
		std::uniform_int_distribution<unsigned> byteValue(0, 255);
		std::array<unsigned, 16> bytes{};
		for (unsigned& value: bytes) {
			value = byteValue(device);
		}
		bytes[6] = (bytes[6] & 0x0FU) | 0x40U; // Version 4.
		bytes[8] = (bytes[8] & 0x3FU) | 0x80U; // RFC 4122 variant.
		std::string out;
		out.reserve(NetMatchConfigUtil::c_WorldIdBytes);
		for (size_t index = 0; index < bytes.size(); ++index) {
			if (index == 4 || index == 6 || index == 8 || index == 10) {
				out += '-';
			}
			out += HexByte(bytes[index]);
		}
		return out;
	}

	std::string NetWorldIdentityFile::DefaultPath() {
		return System::GetWorkingDirectory() + "Worlds/persistent.identity";
	}

	std::string EncodeWorldJoinOffer(const NetWorldCheckpointImage& image) {
		json offer = {
			{"world_id", image.worldId},
			{"boot", image.boot},
			{"round", image.round},
			{"tick", image.tick},
			{"config_revision", image.configRevision},
			{"membership_revision", image.membershipRevision},
			{"match_config_hash", image.matchConfigHash},
			{"module_manifest_hash", image.moduleManifestHash},
			{"digest", image.digest},
			{"path", image.path},
			{"bytes", image.bytes},
			{"capture_ms", image.captureMs},
			{"activation_lead", c_NetWorldActivationLeadFrames},
		};
		return offer.dump();
	}

	bool DecodeWorldJoinOffer(const std::string& text, NetWorldCheckpointImage& out, std::string* error) {
		json parsed = json::parse(text, nullptr, false);
		if (parsed.is_discarded() || !parsed.is_object()) {
			if (error) *error = "world join offer is not an object";
			return false;
		}
		NetWorldCheckpointImage image;
		image.worldId = parsed.value("world_id", std::string());
		image.boot = parsed.value("boot", uint64_t{0});
		image.round = parsed.value("round", uint64_t{0});
		image.tick = parsed.value("tick", uint64_t{0});
		image.configRevision = parsed.value("config_revision", uint64_t{0});
		image.membershipRevision = parsed.value("membership_revision", uint64_t{0});
		image.matchConfigHash = parsed.value("match_config_hash", std::string());
		image.moduleManifestHash = parsed.value("module_manifest_hash", std::string());
		image.digest = parsed.value("digest", std::string());
		image.path = parsed.value("path", std::string());
		image.bytes = parsed.value("bytes", uint64_t{0});
		image.captureMs = parsed.value("capture_ms", 0.0);
		(void)parsed.value("activation_lead", c_NetWorldActivationLeadFrames);
		if (!image.IsValid()) {
			if (error) *error = "world join offer is incomplete";
			return false;
		}
		out = image;
		return true;
	}

	std::string NetWorldIdentityFile::Encode(const NetWorldIdentity& identity) {
		std::ostringstream out;
		out << c_RecordTag << "\n"
		    << "world_id " << identity.worldId << "\n"
		    << "boot " << identity.boot << "\n"
		    << "round " << identity.round << "\n";
		return out.str();
	}

	bool NetWorldIdentityFile::Decode(const std::string& text, NetWorldIdentity& out, std::string* error) {
		std::istringstream in(text);
		std::string tag;
		if (!std::getline(in, tag) || tag.rfind(c_RecordTag, 0) != 0) {
			if (error) *error = "world identity record has no recognised tag";
			return false;
		}
		NetWorldIdentity parsed;
		std::string key;
		while (in >> key) {
			if (key == "world_id") {
				in >> parsed.worldId;
			} else if (key == "boot") {
				in >> parsed.boot;
			} else if (key == "round") {
				in >> parsed.round;
			} else {
				if (error) *error = "world identity record has an unknown field '" + key + "'";
				return false;
			}
		}
		if (!NetMatchConfigUtil::IsWorldId(parsed.worldId)) {
			if (error) *error = "world identity record has no canonical world id";
			return false;
		}
		out = parsed;
		return true;
	}

	bool NetWorldIdentityFile::Peek(const std::string& path, NetWorldIdentity& out, std::string* error) {
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			out = NetWorldIdentity{};
			return true;
		}
		std::ostringstream text;
		text << in.rdbuf();
		return Decode(text.str(), out, error);
	}

	bool NetWorldIdentityFile::OpenForBoot(const std::string& path, NetWorldIdentity& out, std::string* error) {
		std::error_code directoryCode;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), directoryCode);
		if (directoryCode) {
			if (error) *error = "could not create the world identity directory: " + directoryCode.message();
			return false;
		}
		NetWorldIdentity identity;
		if (!Peek(path, identity, error)) {
			return false;
		}
		if (identity.worldId.empty()) {
			identity.worldId = MakeWorldId();
			identity.boot = 0;
			identity.round = 0;
		}
		// The boot is durable BEFORE the caller listens: an old boot's tickets, challenges and
		// uncommitted input are then provably stale whatever happens next.
		++identity.boot;
		++identity.round;
		const std::string temporary = path + ".new";
		{
			std::ofstream write(temporary, std::ios::binary | std::ios::trunc);
			if (!write) {
				if (error) *error = "could not open the world identity record '" + temporary + "'";
				return false;
			}
			write << Encode(identity);
			write.flush();
			if (!write) {
				if (error) *error = "could not write the world identity record";
				return false;
			}
		}
		std::error_code code;
		std::filesystem::rename(temporary, path, code);
		if (code) {
			if (error) *error = "could not publish the world identity record: " + code.message();
			return false;
		}
		out = identity;
		return true;
	}

#pragma endregion

#pragma region Joiner image blob

	namespace {
		void AppendU8(std::vector<uint8_t>& out, uint8_t value) { out.push_back(value); }
		void AppendU16LE(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value));
			out.push_back(static_cast<uint8_t>(value >> 8));
		}
		void AppendU32LE(std::vector<uint8_t>& out, uint32_t value) {
			out.push_back(static_cast<uint8_t>(value));
			out.push_back(static_cast<uint8_t>(value >> 8));
			out.push_back(static_cast<uint8_t>(value >> 16));
			out.push_back(static_cast<uint8_t>(value >> 24));
		}
		void AppendU64LE(std::vector<uint8_t>& out, uint64_t value) {
			AppendU32LE(out, static_cast<uint32_t>(value));
			AppendU32LE(out, static_cast<uint32_t>(value >> 32));
		}

		bool ReadExact(const uint8_t*& cursor, const uint8_t* end, void* dest, size_t size) {
			if (cursor + size > end) {
				return false;
			}
			std::memcpy(dest, cursor, size);
			cursor += size;
			return true;
		}

		uint16_t ReadU16LE(const uint8_t*& cursor, const uint8_t* end, bool& ok) {
			uint16_t value = 0;
			ok = ok && ReadExact(cursor, end, &value, sizeof(value));
			return value;
		}

		uint32_t ReadU32LE(const uint8_t*& cursor, const uint8_t* end, bool& ok) {
			uint32_t value = 0;
			ok = ok && ReadExact(cursor, end, &value, sizeof(value));
			return value;
		}

		uint64_t ReadU64LE(const uint8_t*& cursor, const uint8_t* end, bool& ok) {
			const uint32_t low = ReadU32LE(cursor, end, ok);
			const uint32_t high = ReadU32LE(cursor, end, ok);
			return (static_cast<uint64_t>(high) << 32) | low;
		}
	}

	std::string DigestWorldJoinBytes(const uint8_t* bytes, size_t size) {
		uint64_t hash = 1469598103934665603ULL;
		for (size_t i = 0; i < size; ++i) {
			hash ^= bytes[i];
			hash *= 1099511628211ULL;
		}
		static const char hex[] = "0123456789abcdef";
		std::string out(16, '0');
		for (int i = 7; i >= 0; --i) {
			const unsigned byte = static_cast<unsigned>((hash >> (static_cast<unsigned>(i) * 8U)) & 0xFFU);
			out[static_cast<size_t>((7 - i) * 2)] = hex[(byte >> 4) & 0x0FU];
			out[static_cast<size_t>((7 - i) * 2 + 1)] = hex[byte & 0x0FU];
		}
		return out;
	}

	bool IsWorldJoinImageBlob(const std::vector<uint8_t>& bytes) {
		if (bytes.size() < 5) {
			return false;
		}
		const uint32_t magic = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
		                       (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
		return magic == c_NetWorldImageMagic && bytes[4] == c_NetWorldImageVersion;
	}

	bool EncodeWorldJoinImageBlob(const NetWorldCheckpointImage& image, const std::vector<uint8_t>& archive,
	                              const std::vector<std::vector<uint8_t>>& tail, std::vector<uint8_t>& out, std::string* error) {
		if (!image.IsValid()) {
			if (error) *error = "world join image is incomplete";
			return false;
		}
		const std::string offer = EncodeWorldJoinOffer(image);
		if (offer.size() > 0xFFFFFFFFULL || archive.size() > 0xFFFFFFFFULL || tail.size() > 0xFFFFFFFFULL) {
			if (error) *error = "world join image exceeds the envelope";
			return false;
		}
		out.clear();
		AppendU32LE(out, c_NetWorldImageMagic);
		AppendU8(out, c_NetWorldImageVersion);
		AppendU32LE(out, static_cast<uint32_t>(offer.size()));
		out.insert(out.end(), offer.begin(), offer.end());
		AppendU16LE(out, static_cast<uint16_t>(image.digest.size()));
		out.insert(out.end(), image.digest.begin(), image.digest.end());
		AppendU64LE(out, archive.size());
		out.insert(out.end(), archive.begin(), archive.end());
		AppendU32LE(out, static_cast<uint32_t>(tail.size()));
		for (const std::vector<uint8_t>& frame: tail) {
			if (frame.size() > 0xFFFFFFFFULL) {
				if (error) *error = "a committed tail frame exceeds the envelope";
				return false;
			}
			AppendU32LE(out, static_cast<uint32_t>(frame.size()));
			out.insert(out.end(), frame.begin(), frame.end());
		}
		return true;
	}

	bool DecodeWorldJoinImageBlob(const std::vector<uint8_t>& bytes, NetWorldCheckpointImage& image, std::vector<uint8_t>& archive,
	                              std::vector<std::vector<uint8_t>>& tail, std::string* error) {
		if (!IsWorldJoinImageBlob(bytes)) {
			if (error) *error = "bytes are not a world join image";
			return false;
		}
		const uint8_t* cursor = bytes.data() + 5;
		const uint8_t* end = bytes.data() + bytes.size();
		bool ok = true;
		const uint32_t offerLen = ReadU32LE(cursor, end, ok);
		if (!ok || cursor + offerLen > end) {
			if (error) *error = "world join image offer is truncated";
			return false;
		}
		const std::string offer(reinterpret_cast<const char*>(cursor), offerLen);
		cursor += offerLen;
		NetWorldCheckpointImage decoded;
		if (!DecodeWorldJoinOffer(offer, decoded, error)) {
			return false;
		}
		const uint16_t digestLen = ReadU16LE(cursor, end, ok);
		if (!ok || cursor + digestLen > end) {
			if (error) *error = "world join image digest is truncated";
			return false;
		}
		decoded.digest.assign(reinterpret_cast<const char*>(cursor), digestLen);
		cursor += digestLen;
		const uint64_t archiveLen = ReadU64LE(cursor, end, ok);
		if (!ok || archiveLen > static_cast<uint64_t>(end - cursor)) {
			if (error) *error = "world join image archive is truncated";
			return false;
		}
		archive.assign(cursor, cursor + static_cast<size_t>(archiveLen));
		cursor += static_cast<size_t>(archiveLen);
		const uint32_t tailCount = ReadU32LE(cursor, end, ok);
		if (!ok) {
			if (error) *error = "world join image tail is truncated";
			return false;
		}
		tail.clear();
		tail.reserve(tailCount);
		for (uint32_t i = 0; i < tailCount; ++i) {
			const uint32_t frameLen = ReadU32LE(cursor, end, ok);
			if (!ok || cursor + frameLen > end) {
				if (error) *error = "world join image tail frame is truncated";
				return false;
			}
			tail.emplace_back(cursor, cursor + frameLen);
			cursor += frameLen;
		}
		if (cursor != end) {
			if (error) *error = "world join image has trailing bytes";
			return false;
		}
		if (DigestWorldJoinBytes(archive) != decoded.digest) {
			if (error) *error = "world join image digest does not match the archive";
			return false;
		}
		image = std::move(decoded);
		return true;
	}

	NetLobbyStateChunk MakeWorldJoinReport(uint8_t kind, uint64_t value) {
		NetLobbyStateChunk chunk;
		chunk.transferId = c_NetWorldReportTransferId;
		chunk.totalBytes = 9;
		chunk.chunkIndex = 0;
		chunk.chunkCount = 1;
		chunk.bytes.resize(9);
		chunk.bytes[0] = kind;
		for (int i = 0; i < 8; ++i) {
			chunk.bytes[static_cast<size_t>(i + 1)] = static_cast<uint8_t>(value >> (8 * i));
		}
		return chunk;
	}

	bool ParseWorldJoinReport(const NetLobbyStateChunk& chunk, uint8_t& kind, uint64_t& value) {
		if (chunk.transferId != c_NetWorldReportTransferId || chunk.bytes.size() != 9 || chunk.totalBytes != 9 ||
		    chunk.chunkCount != 1 || chunk.chunkIndex != 0) {
			return false;
		}
		kind = chunk.bytes[0];
		if (kind != c_NetWorldReportProgress && kind != c_NetWorldReportCatchUp && kind != c_NetWorldReportActivate) {
			return false;
		}
		value = 0;
		for (int i = 0; i < 8; ++i) {
			value |= static_cast<uint64_t>(chunk.bytes[static_cast<size_t>(i + 1)]) << (8 * i);
		}
		return true;
	}

	NetGameWorldTransition BuildWorldActivateTransition(const NetWorldJoinSession& session, const NetMatchConfig& config, uint64_t membershipRevision) {
		NetGameWorldTransition transition;
		transition.kind = NetGameWorldTransition::Activate;
		transition.peerId = session.assignedPeerId;
		transition.holderGeneration = session.holderGeneration;
		transition.membershipRevision = membershipRevision;
		transition.activationFrame = session.activationTick;
		transition.team = session.team;
		int human = 0;
		transition.player = 0;
		for (const NetMatchPlayerSlot& slot: config.players) {
			if (slot.cpu) {
				continue;
			}
			if (slot.peerId == session.assignedPeerId) {
				transition.player = human;
				break;
			}
			++human;
		}
		transition.bindBrain = !session.spectator && session.assignedPeerId != 0;
		if (!session.spectator) {
			transition.className = "AHuman";
			transition.preset = "Brain Robot";
			transition.module = "Base.rte";
			transition.aiMode = 0;
			switch (session.team) {
				case 1: transition.posX = 1120.0F; break;
				case 2: transition.posX = 640.0F; break;
				case 3: transition.posX = 1360.0F; break;
				default: transition.posX = 880.0F; break;
			}
			transition.posY = 0.0F;
		}
		return transition;
	}

#pragma endregion

#pragma region Committed frame tail

	void NetWorldFrameLog::Configure(size_t maxFrames, uint64_t maxBytes) {
		m_MaxFrames = maxFrames == 0 ? c_DefaultMaxFrames : maxFrames;
		m_MaxBytes = maxBytes == 0 ? c_DefaultMaxBytes : maxBytes;
		Trim();
	}

	bool NetWorldFrameLog::Append(const NetLockstepFrame& frame, std::string* error) {
		if (!m_Records.empty() && frame.targetFrame != m_Records.back().frame + 1) {
			if (error) *error = "the committed tail cannot skip from frame " + std::to_string(m_Records.back().frame) + " to " + std::to_string(frame.targetFrame);
			return false;
		}
		Record record;
		record.frame = frame.targetFrame;
		NetLockstepError encodeError;
		if (!NetLockstepCodec::EncodeRecoveryInput(frame, record.bytes, &encodeError)) {
			if (error) *error = "the committed tail could not encode frame " + std::to_string(frame.targetFrame) + ": " + encodeError.message;
			return false;
		}
		m_Bytes += record.bytes.size();
		m_Records.push_back(std::move(record));
		Trim();
		return true;
	}

	void NetWorldFrameLog::Trim() {
		while (!m_Records.empty() && (m_Records.size() > m_MaxFrames || m_Bytes > m_MaxBytes)) {
			m_Bytes -= m_Records.front().bytes.size();
			m_Records.pop_front();
			++m_Evicted;
		}
	}

	bool NetWorldFrameLog::Covers(uint64_t frame) const {
		return !m_Records.empty() && frame >= m_Records.front().frame && frame <= m_Records.back().frame;
	}

	size_t NetWorldFrameLog::CopyFrom(uint64_t from, size_t maxRecords, uint64_t maxBytes, std::vector<std::vector<uint8_t>>& out) const {
		out.clear();
		uint64_t bytes = 0;
		for (const Record& record: m_Records) {
			if (record.frame < from) {
				continue;
			}
			if (out.size() >= maxRecords || (bytes != 0 && bytes + record.bytes.size() > maxBytes)) {
				break;
			}
			bytes += record.bytes.size();
			out.push_back(record.bytes);
		}
		return out.size();
	}

	void NetWorldFrameLog::DropThrough(uint64_t frame) {
		while (!m_Records.empty() && m_Records.front().frame <= frame) {
			m_Bytes -= m_Records.front().bytes.size();
			m_Records.pop_front();
		}
	}

	void NetWorldFrameLog::Clear() {
		m_Records.clear();
		m_Bytes = 0;
		m_Evicted = 0;
	}

#pragma endregion

#pragma region Measurements

	void NetWorldMetrics::NoteCapture(double stallMs, uint64_t bytes) {
		++m_Captures;
		m_CaptureBytes += bytes;
		m_CaptureMaxMs = std::max(m_CaptureMaxMs, stallMs);
		if (stallMs > c_CaptureCeilingMs) {
			++m_CaptureCeilingMisses;
		}
		if (m_CaptureStalls.size() < c_MaxSamples) {
			m_CaptureStalls.push_back(stallMs);
		} else {
			// A full reservoir keeps the worst samples: a percentile read off the tail is the number
			// the bound is judged on, and dropping the tail would flatter it.
			const auto smallest = std::min_element(m_CaptureStalls.begin(), m_CaptureStalls.end());
			if (smallest != m_CaptureStalls.end() && *smallest < stallMs) {
				*smallest = stallMs;
			}
		}
	}

	void NetWorldMetrics::NoteCatchUp(uint64_t ticks, uint64_t elapsedMs) {
		m_CatchUpTicks += ticks;
		m_CatchUpMs += elapsedMs;
	}

	void NetWorldMetrics::NoteTransfer(uint64_t bytes) {
		m_TransferBytes += bytes;
	}

	void NetWorldMetrics::NotePurity(uint64_t tick, uint64_t withCapture, uint64_t withoutCapture) {
		++m_PurityProbes;
		if (withCapture != withoutCapture) {
			++m_PurityMismatches;
			if (m_FirstPurityMismatchTick == 0) {
				m_FirstPurityMismatchTick = tick;
			}
		}
	}

	double NetWorldMetrics::CapturePercentileMs(double percentile) const {
		if (m_CaptureStalls.empty()) {
			return 0.0;
		}
		std::vector<double> sorted = m_CaptureStalls;
		std::sort(sorted.begin(), sorted.end());
		const double clamped = std::clamp(percentile, 0.0, 1.0);
		const size_t index = static_cast<size_t>(std::llround(clamped * static_cast<double>(sorted.size() - 1)));
		return sorted[index];
	}

	double NetWorldMetrics::CatchUpRatio() const {
		if (m_CatchUpMs == 0) {
			return 0.0;
		}
		// Ticks the joiner replayed per tick the world produced in the same wall time.
		const double worldTicks = static_cast<double>(m_CatchUpMs) / c_CaptureCeilingMs;
		return worldTicks == 0.0 ? 0.0 : static_cast<double>(m_CatchUpTicks) / worldTicks;
	}

	std::string NetWorldMetrics::BuildReportJson() const {
		json report = {
			{"captures", m_Captures},
			{"capture_bytes", m_CaptureBytes},
			{"capture_ceiling_ms", c_CaptureCeilingMs},
			{"capture_ceiling_misses", m_CaptureCeilingMisses},
			{"capture_p50_ms", CapturePercentileMs(0.50)},
			{"capture_p95_ms", CapturePercentileMs(0.95)},
			{"capture_p99_ms", CapturePercentileMs(0.99)},
			{"capture_max_ms", m_CaptureMaxMs},
			{"catch_up_ticks", m_CatchUpTicks},
			{"catch_up_ms", m_CatchUpMs},
			{"catch_up_ratio", CatchUpRatio()},
			{"transfer_bytes", m_TransferBytes},
			{"purity_probes", m_PurityProbes},
			{"purity_mismatches", m_PurityMismatches},
			{"first_purity_mismatch_tick", m_FirstPurityMismatchTick},
		};
		return report.dump();
	}

	void NetWorldMetrics::Reset() {
		*this = NetWorldMetrics{};
	}

#pragma endregion

#pragma region Membership

	bool NetWorldMembership::Configure(const NetMatchConfig& config, std::string* error) {
		if (!config.persistentWorld) {
			if (error) *error = "only a persistent world has world slots";
			return false;
		}
		if (config.peerCount < 1 || config.peerCount > NetMatchConfigUtil::c_MaxPeerCount) {
			if (error) *error = "the world's peer capacity is out of range";
			return false;
		}
		m_Slots.clear();
		// The order is the config's, so every peer and every boot names the same slot for the same
		// lockstep id; a live peer count never re-derives it.
		for (uint8_t peerId = 1; peerId <= config.peerCount; ++peerId) {
			if (peerId == config.hostPeerId) {
				continue;
			}
			NetWorldSlot slot;
			slot.peerId = peerId;
			slot.team = -1;
			for (const NetMatchPlayerSlot& player: config.players) {
				if (!player.cpu && player.peerId == peerId) {
					slot.team = static_cast<int8_t>(player.team);
					break;
				}
			}
			if (slot.team < 0) {
				// A capacity slot the roster does not spell out takes the next team the CPU does not hold.
				std::array<bool, 4> taken{};
				for (const NetMatchPlayerSlot& player: config.players) {
					if (player.team < 4) {
						taken[player.team] = taken[player.team] || player.cpu;
					}
				}
				for (const NetWorldSlot& placed: m_Slots) {
					if (placed.team >= 0 && placed.team < 4) {
						taken[placed.team] = true;
					}
				}
				for (size_t team = 0; team < taken.size(); ++team) {
					if (!taken[team]) {
						slot.team = static_cast<int8_t>(team);
						break;
					}
				}
			}
			if (slot.team < 0) {
				if (error) *error = "the world has no team left for peer " + std::to_string(static_cast<int>(peerId));
				return false;
			}
			slot.generation = 1;
			m_Slots.push_back(slot);
		}
		if (m_Slots.empty()) {
			if (error) *error = "the world offers no gameplay slot";
			return false;
		}
		m_Revision = 1;
		return true;
	}

	NetWorldSlot* NetWorldMembership::Find(uint8_t peerId) {
		const auto found = std::find_if(m_Slots.begin(), m_Slots.end(), [&](const NetWorldSlot& slot) { return slot.peerId == peerId; });
		return found == m_Slots.end() ? nullptr : &*found;
	}

	const NetWorldSlot* NetWorldMembership::FirstFreeSlot() const {
		const auto found = std::find_if(m_Slots.begin(), m_Slots.end(), [](const NetWorldSlot& slot) { return !slot.held; });
		return found == m_Slots.end() ? nullptr : &*found;
	}

	const NetWorldSlot* NetWorldMembership::SlotOfSeat(uint16_t stableSeat) const {
		if (stableSeat == 0) {
			return nullptr;
		}
		const auto found = std::find_if(m_Slots.begin(), m_Slots.end(), [&](const NetWorldSlot& slot) { return slot.stableSeat == stableSeat; });
		return found == m_Slots.end() ? nullptr : &*found;
	}

	bool NetWorldMembership::Hold(uint8_t peerId, uint16_t stableSeat, const std::string& holderName, std::string* error) {
		NetWorldSlot* slot = Find(peerId);
		if (slot == nullptr) {
			if (error) *error = "peer " + std::to_string(static_cast<int>(peerId)) + " is not a world slot";
			return false;
		}
		if (slot->held) {
			if (error) *error = "world slot " + std::to_string(static_cast<int>(peerId)) + " is already held";
			return false;
		}
		slot->held = true;
		slot->stableSeat = stableSeat;
		slot->holderName = holderName;
		++m_Revision;
		return true;
	}

	bool NetWorldMembership::Release(uint8_t peerId, std::string* error) {
		NetWorldSlot* slot = Find(peerId);
		if (slot == nullptr) {
			if (error) *error = "peer " + std::to_string(static_cast<int>(peerId)) + " is not a world slot";
			return false;
		}
		if (!slot->held) {
			if (error) *error = "world slot " + std::to_string(static_cast<int>(peerId)) + " is not held";
			return false;
		}
		slot->held = false;
		slot->stableSeat = 0;
		slot->holderName.clear();
		// The next holder of this slot is a new one: an old ticket cannot pass for this generation.
		++slot->generation;
		++m_Revision;
		return true;
	}

	size_t NetWorldMembership::FreeSlots() const {
		return static_cast<size_t>(std::count_if(m_Slots.begin(), m_Slots.end(), [](const NetWorldSlot& slot) { return !slot.held; }));
	}

	size_t NetWorldMembership::HeldSlots() const {
		return m_Slots.size() - FreeSlots();
	}

	std::string NetWorldMembership::BuildReportJson() const {
		json slots = json::array();
		for (const NetWorldSlot& slot: m_Slots) {
			slots.push_back({{"peer_id", static_cast<int>(slot.peerId)}, {"team", static_cast<int>(slot.team)},
			                 {"generation", slot.generation}, {"stable_seat", slot.stableSeat},
			                 {"held", slot.held}, {"holder", slot.holderName}});
		}
		json report = {{"revision", m_Revision}, {"slots", std::move(slots)}, {"free", FreeSlots()}};
		return report.dump(-1, ' ', false, json::error_handler_t::replace);
	}

#pragma endregion

#pragma region Join host

	bool NetWorldJoinHost::Configure(const NetMatchConfig& config, const NetWorldIdentity& identity, std::string* error) {
		if (!identity.IsValid()) {
			if (error) *error = "the world identity is incomplete";
			return false;
		}
		if (!config.persistentWorld || config.worldId != identity.worldId) {
			if (error) *error = "the match config does not describe this world";
			return false;
		}
		if (!m_Membership.Configure(config, error)) {
			return false;
		}
		m_Identity = identity;
		m_Config = config;
		m_Sessions.clear();
		m_Image = NetWorldCheckpointImage{};
		m_Tail.Clear();
		return true;
	}

	NetWorldJoinSession* NetWorldJoinHost::Find(NetPeerId connection) {
		const auto found = std::find_if(m_Sessions.begin(), m_Sessions.end(), [&](const NetWorldJoinSession& session) { return session.connection == connection; });
		return found == m_Sessions.end() ? nullptr : &*found;
	}

	const NetWorldJoinSession* NetWorldJoinHost::FindSession(NetPeerId connection) const {
		const auto found = std::find_if(m_Sessions.begin(), m_Sessions.end(), [&](const NetWorldJoinSession& session) { return session.connection == connection; });
		return found == m_Sessions.end() ? nullptr : &*found;
	}

	bool NetWorldJoinHost::BeginJoin(NetPeerId connection, uint16_t stableSeat, const std::string& holderName, uint64_t nowMs, std::string* error) {
		if (!IsConfigured()) {
			if (error) *error = "the world join plane is not configured";
			return false;
		}
		if (connection == c_InvalidNetPeerId) {
			if (error) *error = "a world join needs a connection";
			return false;
		}
		if (Find(connection) != nullptr) {
			if (error) *error = "that connection already has a world bootstrap";
			return false;
		}
		NetWorldJoinSession session;
		session.connection = connection;
		session.stableSeat = stableSeat;
		session.openedAtMs = nowMs;
		session.phase = NetWorldJoinPhase::Authenticating;
		// A credentialed holder of an existing seat outranks a fresh allocation for the same slot.
		const NetWorldSlot* slot = m_Membership.SlotOfSeat(stableSeat);
		if (slot == nullptr) {
			slot = m_Membership.FirstFreeSlot();
			if (slot == nullptr) {
				// A full world admits an authority-free spectator instead of refusing the connection.
				session.spectator = true;
				session.phase = NetWorldJoinPhase::SnapshotTransfer;
				m_Sessions.push_back(std::move(session));
				return true;
			}
			const uint8_t peerId = slot->peerId;
			const int8_t team = slot->team;
			const uint32_t generation = slot->generation;
			if (!m_Membership.Hold(peerId, stableSeat, holderName, error)) {
				return false;
			}
			session.assignedPeerId = peerId;
			session.team = team;
			session.holderGeneration = generation;
			session.phase = NetWorldJoinPhase::SnapshotTransfer;
			m_Sessions.push_back(std::move(session));
			return true;
		}
		session.assignedPeerId = slot->peerId;
		session.team = slot->team;
		session.holderGeneration = slot->generation;
		session.phase = NetWorldJoinPhase::SnapshotTransfer;
		m_Sessions.push_back(std::move(session));
		return true;
	}

	void NetWorldJoinHost::PublishImage(const NetWorldCheckpointImage& image) {
		m_Image = image;
		m_Metrics.NoteCapture(image.captureMs, image.bytes);
		for (NetWorldJoinSession& session: m_Sessions) {
			if (session.phase == NetWorldJoinPhase::SnapshotTransfer && session.snapshotTick == 0) {
				session.snapshotTick = image.tick;
				session.deliveredThrough = image.tick;
				session.acknowledgedThrough = image.tick;
			}
		}
	}

	bool NetWorldJoinHost::NoteTransferProgress(NetPeerId connection, uint16_t ackedChunks, uint16_t totalChunks) {
		NetWorldJoinSession* session = Find(connection);
		if (session == nullptr) {
			return false;
		}
		session->ackedChunks = ackedChunks;
		session->totalChunks = totalChunks;
		return true;
	}

	bool NetWorldJoinHost::NoteTransferStarted(NetPeerId connection, uint64_t transferId, uint16_t totalChunks, uint64_t deliveredThrough) {
		NetWorldJoinSession* session = Find(connection);
		if (session == nullptr) {
			return false;
		}
		session->transferStarted = true;
		session->transferId = transferId;
		session->totalChunks = totalChunks;
		session->deliveredThrough = deliveredThrough;
		return true;
	}

	bool NetWorldJoinHost::NoteDeliveredThrough(NetPeerId connection, uint64_t frame) {
		NetWorldJoinSession* session = Find(connection);
		if (session == nullptr) {
			return false;
		}
		if (frame > session->deliveredThrough) {
			session->deliveredThrough = frame;
		}
		return true;
	}

	bool NetWorldJoinHost::NoteTransferComplete(NetPeerId connection, uint64_t bytes, std::string* error) {
		NetWorldJoinSession* session = Find(connection);
		if (session == nullptr) {
			if (error) *error = "no world bootstrap for that connection";
			return false;
		}
		if (session->snapshotTick == 0) {
			if (error) *error = "the bootstrap has no checkpoint image yet";
			return false;
		}
		session->phase = NetWorldJoinPhase::CatchingUp;
		session->transferBytes = bytes;
		m_Metrics.NoteTransfer(bytes);
		return true;
	}

	bool NetWorldJoinHost::NoteCatchUpProgress(NetPeerId connection, uint64_t appliedThrough, uint64_t ticksReplayed, uint64_t elapsedMs, uint64_t nowFrame, uint64_t* outActivationTick, std::string* error) {
		if (outActivationTick) *outActivationTick = 0;
		NetWorldJoinSession* session = Find(connection);
		if (session == nullptr) {
			if (error) *error = "no world bootstrap for that connection";
			return false;
		}
		if (session->phase != NetWorldJoinPhase::CatchingUp) {
			if (error) *error = "that bootstrap is not catching up";
			return false;
		}
		if (appliedThrough < session->acknowledgedThrough) {
			if (error) *error = "a catch-up acknowledgement cannot go backwards";
			return false;
		}
		session->acknowledgedThrough = appliedThrough;
		session->catchUpTicks += ticksReplayed;
		session->catchUpMs += elapsedMs;
		m_Metrics.NoteCatchUp(ticksReplayed, elapsedMs);
		if (session->activationTick != 0 || session->spectator) {
			return true;
		}
		// The world keeps producing while the joiner replays, so activation waits until the joiner is
		// inside the lead and can have its pipeline primed before its first required frame.
		if (appliedThrough + c_NetWorldActivationLeadFrames < nowFrame) {
			return true;
		}
		session->activationTick = nowFrame + c_NetWorldActivationLeadFrames;
		if (outActivationTick) *outActivationTick = session->activationTick;
		return true;
	}

	const NetWorldJoinSession* NetWorldJoinHost::DueActivation(uint64_t nowFrame) const {
		const auto found = std::find_if(m_Sessions.begin(), m_Sessions.end(), [&](const NetWorldJoinSession& session) {
			return session.phase == NetWorldJoinPhase::CatchingUp && session.activationTick != 0 && session.activationTick <= nowFrame &&
			       session.acknowledgedThrough + 1 >= session.activationTick;
		});
		return found == m_Sessions.end() ? nullptr : &*found;
	}

	const NetWorldJoinSession* NetWorldJoinHost::SlowActivation(uint64_t nowFrame) const {
		const auto found = std::find_if(m_Sessions.begin(), m_Sessions.end(), [&](const NetWorldJoinSession& session) {
			return session.phase == NetWorldJoinPhase::CatchingUp && session.activationTick != 0 && nowFrame > session.activationTick &&
			       session.acknowledgedThrough + 1 < session.activationTick;
		});
		return found == m_Sessions.end() ? nullptr : &*found;
	}

	bool NetWorldJoinHost::ReannounceActivation(NetPeerId connection, uint64_t nowFrame, uint64_t* outActivationTick, std::string* error) {
		if (outActivationTick) *outActivationTick = 0;
		NetWorldJoinSession* session = Find(connection);
		if (session == nullptr) {
			if (error) *error = "no world bootstrap for that connection";
			return false;
		}
		if (session->phase != NetWorldJoinPhase::CatchingUp || session->activationTick == 0) {
			if (error) *error = "that bootstrap has no activation to move";
			return false;
		}
		if (session->activationReannounces >= c_NetWorldActivationReannounceLimit) {
			if (error) *error = "the joiner already missed a re-announced activation";
			return false;
		}
		session->activationTick = nowFrame + c_NetWorldActivationLeadFrames;
		++session->activationReannounces;
		if (outActivationTick) *outActivationTick = session->activationTick;
		return true;
	}

	bool NetWorldJoinHost::CompleteActivation(NetPeerId connection, uint64_t atFrame, std::string* error) {
		NetWorldJoinSession* session = Find(connection);
		if (session == nullptr) {
			if (error) *error = "no world bootstrap for that connection";
			return false;
		}
		if (session->activationTick == 0 || atFrame < session->activationTick) {
			if (error) *error = "that bootstrap has no activation due at frame " + std::to_string(atFrame);
			return false;
		}
		session->phase = NetWorldJoinPhase::Active;
		++m_ActivationsCommitted;
		return true;
	}

	void NetWorldJoinHost::CancelJoin(NetPeerId connection, const std::string& reason) {
		NetWorldJoinSession* session = Find(connection);
		if (session == nullptr) {
			return;
		}
		// A bootstrap that never activated holds no seat the round waits on: releasing it is not a
		// departure and must not reach the dropped-seat hold.
		if (session->phase != NetWorldJoinPhase::Active && session->assignedPeerId != 0) {
			(void)m_Membership.Release(session->assignedPeerId, nullptr);
		}
		session->phase = NetWorldJoinPhase::Failed;
		session->refusal = reason;
		++m_JoinsCancelled;
		std::erase_if(m_Sessions, [&](const NetWorldJoinSession& entry) { return entry.connection == connection; });
	}

	size_t NetWorldJoinHost::ExpireStaleJoins(uint64_t nowMs) {
		std::vector<NetPeerId> stale;
		for (const NetWorldJoinSession& session: m_Sessions) {
			if (session.phase == NetWorldJoinPhase::Active || session.openedAtMs == 0) {
				continue;
			}
			if (nowMs > session.openedAtMs && nowMs - session.openedAtMs > c_NetWorldJoinDeadlineMs) {
				stale.push_back(session.connection);
			}
		}
		for (const NetPeerId connection: stale) {
			CancelJoin(connection, "the world join deadline expired");
		}
		return stale.size();
	}

	uint64_t NetWorldJoinHost::OldestNeededFrame() const {
		uint64_t oldest = 0;
		for (const NetWorldJoinSession& session: m_Sessions) {
			if (session.phase != NetWorldJoinPhase::CatchingUp && session.phase != NetWorldJoinPhase::SnapshotTransfer) {
				continue;
			}
			const uint64_t needed = session.acknowledgedThrough + 1;
			if (oldest == 0 || needed < oldest) {
				oldest = needed;
			}
		}
		return oldest;
	}

	std::string NetWorldJoinHost::BuildReportJson() const {
		json sessions = json::array();
		for (const NetWorldJoinSession& session: m_Sessions) {
			sessions.push_back({{"connection", session.connection}, {"phase", NetWorldJoinPhaseName(session.phase)},
			                    {"peer_id", static_cast<int>(session.assignedPeerId)}, {"team", static_cast<int>(session.team)},
			                    {"spectator", session.spectator}, {"snapshot_tick", session.snapshotTick},
			                    {"activation_tick", session.activationTick}, {"applied_through", session.acknowledgedThrough},
			                    {"transfer_bytes", session.transferBytes}});
		}
		json report = {
			{"world_id", m_Identity.worldId},
			{"boot", m_Identity.boot},
			{"round", m_Identity.round},
			{"activations", m_ActivationsCommitted},
			{"joins_cancelled", m_JoinsCancelled},
			{"image", {{"tick", m_Image.tick}, {"bytes", m_Image.bytes}, {"digest", m_Image.digest}, {"capture_ms", m_Image.captureMs}}},
			{"tail", {{"first", m_Tail.FirstFrame()}, {"last", m_Tail.LastFrame()}, {"count", m_Tail.Count()}, {"bytes", m_Tail.Bytes()}, {"evicted", m_Tail.Evicted()}}},
			{"sessions", std::move(sessions)},
		};
		report["membership"] = json::parse(m_Membership.BuildReportJson(), nullptr, false);
		report["metrics"] = json::parse(m_Metrics.BuildReportJson(), nullptr, false);
		return report.dump(-1, ' ', false, json::error_handler_t::replace);
	}

	void NetWorldJoinHost::Reset() {
		m_Identity = NetWorldIdentity{};
		m_Config = NetMatchConfig{};
		m_Sessions.clear();
		m_Image = NetWorldCheckpointImage{};
		m_Tail.Clear();
		m_Metrics.Reset();
		m_ActivationsCommitted = 0;
		m_JoinsCancelled = 0;
	}

#pragma endregion

} // namespace RTE
