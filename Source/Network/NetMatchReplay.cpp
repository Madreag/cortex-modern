#include "NetMatchReplay.h"
#include "TelemetryBundle.h"
#include "System.h"

#include "NetLobbyProtocol.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace RTE {

	namespace {
		struct ReplayWorkTimer {
			uint64_t frame;
			const char* phase;
			size_t bytes = 0;
			std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
			~ReplayWorkTimer() {
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
				if (ms >= 10) System::PrintDiagnosticLine("[net-replay-work] frame=" + std::to_string(frame) + " phase=" + phase + " ms=" + std::to_string(ms) + " bytes=" + std::to_string(bytes));
			}
		};

		void AppendU16(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
			for (int shift = 0; shift < 32; shift += 8) {
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
			}
		}

		void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
			for (int shift = 0; shift < 64; shift += 8) {
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
			}
		}

		void AppendString(std::vector<uint8_t>& out, const std::string& value) {
			out.push_back(static_cast<uint8_t>(value.size()));
			out.insert(out.end(), value.begin(), value.end());
		}

		bool ReadU64(std::ifstream& in, uint64_t& outValue) {
			uint8_t bytes[8];
			if (!in.read(reinterpret_cast<char*>(bytes), 8)) {
				return false;
			}
			outValue = 0;
			for (int i = 7; i >= 0; --i) {
				outValue = (outValue << 8) | static_cast<uint64_t>(bytes[i]);
			}
			return true;
		}

		bool ReadString(std::ifstream& in, size_t maxBytes, std::string& outValue) {
			uint8_t length = 0;
			if (!in.read(reinterpret_cast<char*>(&length), 1) || length > maxBytes) {
				return false;
			}
			outValue.assign(length, '\0');
			return length == 0 || static_cast<bool>(in.read(outValue.data(), length));
		}

		uint32_t ReadU32(const uint8_t* bytes) {
			return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
			       (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
		}

		bool ReadU32(std::ifstream& in, uint32_t& outValue) {
			uint8_t bytes[4];
			if (!in.read(reinterpret_cast<char*>(bytes), 4)) {
				return false;
			}
			outValue = ReadU32(bytes);
			return true;
		}

		size_t LockstepPacketSize(const uint8_t* data, size_t size) {
			if (size < NetLockstepCodec::c_HeaderBytes) {
				return 0;
			}
			const uint32_t payload = ReadU32(data + 12);
			const size_t total = static_cast<size_t>(NetLockstepCodec::c_HeaderBytes) + payload;
			return total > size ? 0 : total;
		}

		bool EncodeSenderPacket(const NetLockstepFrame& record, std::vector<uint8_t>& out, std::string* error) {
			auto firstTiming = record.commands.end();
			while (firstTiming != record.commands.begin()) {
				const auto& command = *std::prev(firstTiming);
				if (!std::holds_alternative<NetGameSeatHold>(command.payload) && !std::holds_alternative<NetGameInputDelay>(command.payload) && !std::holds_alternative<NetGameSeatReclaim>(command.payload) &&
				    !std::holds_alternative<NetGameSeatRelease>(command.payload)) break;
				--firstTiming;
			}
			if (firstTiming != record.commands.end()) {
				NetLockstepFrame inputs = record;
				inputs.commands.erase(inputs.commands.begin() + std::distance(record.commands.begin(), firstTiming), inputs.commands.end());
				if (!EncodeSenderPacket(inputs, out, error)) return false;
				NetLockstepFrame timing;
				timing.senderPeerId = record.senderPeerId; timing.targetFrame = record.targetFrame; timing.roundId = record.roundId;
				timing.commands.assign(firstTiming, record.commands.end());
				std::vector<uint8_t> bytes;
				NetLockstepError failure;
				if (!NetLockstepCodec::Encode({timing}, bytes, &failure)) { if (error) *error = failure.message; return false; }
				out.insert(out.end(), bytes.begin(), bytes.end());
				return true;
			}
			NetLockstepError codecError;
			size_t observationsEncoded = record.observations.size();
			size_t valueObservationsEncoded = record.valueObservations.size();
			if (!NetLockstepCodec::Encode({record}, out, &codecError, nullptr, &observationsEncoded, &valueObservationsEncoded)) {
				if (error) *error = "could not encode a replay frame: " + codecError.message;
				return false;
			}
			if (observationsEncoded < record.observations.size()) {
				if (error) *error = "a replay frame's sound observations do not fit one record";
				return false;
			}
			if (valueObservationsEncoded < record.valueObservations.size()) {
				if (error) *error = "a replay frame's value observations do not fit one record";
				return false;
			}
			return true;
		}
	} // namespace

	struct NetMatchReplayWriter::WriteState {
		struct Record {
			std::vector<uint8_t> prefix;
			std::vector<uint8_t> payload;
			std::function<void()> beforeWrite;
			uint64_t ordinal = 0;
		};
		std::string path;
		std::vector<uint8_t> header;
		std::deque<std::shared_ptr<const Record>> records;
		size_t pendingBytes = 0;
		bool stop = false;
		bool finished = false;
		std::string error;
	};

	struct NetMatchReplayWriter::StorageState {
		std::mutex mutex;
		std::condition_variable ready;
		std::deque<std::shared_ptr<WriteState>> segments;
		size_t pendingBytes = 0;
		bool stop = false;
	};

	NetMatchReplayWriter::NetMatchReplayWriter() = default;

	NetMatchReplayWriter::~NetMatchReplayWriter() {
		Close();
		(void)WaitForClose(c_ExitDrainMs);
		if (m_Storage) {
			{ std::lock_guard lock(m_Storage->mutex); m_Storage->stop = true; }
			m_Storage->ready.notify_one();
		}
	}

	std::string NetMatchReplayWriter::GetWriteError() const {
		if (!m_Writes || !m_Storage) return {};
		std::lock_guard lock(m_Storage->mutex);
		return m_Writes->error;
	}

	bool NetMatchReplayWriter::IsCloseComplete() const {
		if (!m_Writes || !m_Storage) return true;
		std::lock_guard lock(m_Storage->mutex);
		return m_Writes->finished;
	}

	bool NetMatchReplayWriter::WaitForClose(uint64_t timeoutMs) const {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (!IsCloseComplete()) {
			if (std::chrono::steady_clock::now() >= deadline) return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}

	void NetMatchReplayWriter::WriteQueuedRecords(std::shared_ptr<StorageState> storage) {
		for (;;) {
			std::shared_ptr<WriteState> state;
			{
				std::unique_lock lock(storage->mutex);
				storage->ready.wait(lock, [&] { return storage->stop || !storage->segments.empty(); });
				if (storage->segments.empty()) return;
				state = std::move(storage->segments.front());
				storage->segments.pop_front();
			}
			std::ofstream out(state->path, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(state->header.data()), static_cast<std::streamsize>(state->header.size()));
			{
				std::lock_guard lock(storage->mutex);
				storage->pendingBytes -= state->header.size();
				state->pendingBytes -= state->header.size();
				state->header.clear();
				if (!out) state->error = "could not open or write the replay header: " + state->path;
			}
			for (;;) {
				std::shared_ptr<const WriteState::Record> record;
				{
					std::unique_lock lock(storage->mutex);
					storage->ready.wait(lock, [&] { return state->stop || !state->error.empty() || !state->records.empty(); });
					if (!state->error.empty() || state->records.empty()) break;
					record = std::move(state->records.front());
					state->records.pop_front();
				}
				std::string failure;
				try {
					if (record->beforeWrite) record->beforeWrite();
					ReplayWorkTimer work{record->ordinal, "write", record->payload.size()};
					out.write(reinterpret_cast<const char*>(record->prefix.data()), static_cast<std::streamsize>(record->prefix.size()));
					out.write(reinterpret_cast<const char*>(record->payload.data()), static_cast<std::streamsize>(record->payload.size()));
					if (!out) failure = "could not write a replay record";
				} catch (const std::exception& exception) {
					failure = exception.what();
				}
				{
					std::lock_guard lock(storage->mutex);
					storage->pendingBytes -= record->prefix.size() + record->payload.size();
					state->pendingBytes -= record->prefix.size() + record->payload.size();
					if (!failure.empty()) state->error = std::move(failure);
				}
			}
			bool clean = false;
			std::deque<std::shared_ptr<const WriteState::Record>> discarded;
			{
				std::lock_guard lock(storage->mutex);
				clean = state->error.empty();
				storage->pendingBytes -= state->pendingBytes;
				state->pendingBytes = 0;
				discarded.swap(state->records);
			}
			if (clean && out.good()) {
				std::vector<uint8_t> endMarker;
				AppendU32(endMarker, c_EndMarker);
				out.write(reinterpret_cast<const char*>(endMarker.data()), static_cast<std::streamsize>(endMarker.size()));
			}
			out.close();
			std::string failure;
			{
				std::lock_guard lock(storage->mutex);
				if (!out && state->error.empty()) state->error = "could not finish the replay file";
				failure = state->error;
				state->finished = true;
			}
			if (!failure.empty()) System::PrintDiagnosticLine("[net-match] replay recording stopped: " + failure + " path=" + state->path);
		}
	}

	bool NetMatchReplayWriter::Open(const std::string& path, const NetMatchConfig& config, std::string* error) {
		return Open(path, config, nullptr, error);
	}

	bool NetMatchReplayWriter::Open(const std::string& path, const NetMatchConfig& config, const NetWorldSegmentHeader* segment, std::string* error) {
		Close();

		if (segment && (segment->worldId.empty() || segment->worldId.size() > c_MaxSegmentFieldBytes ||
		                segment->worldDigest.size() > c_MaxSegmentFieldBytes || segment->tick == 0)) {
			if (error) *error = "invalid world segment header";
			return false;
		}
		m_DiagnosticBytes.clear();
		m_DiagnosticFrames = 0;
		m_DiagnosticTruncated = false;
		m_AgreedStart.reset();
		m_AgreedStartWritten = false;
		// The synced config rides the lobby codec, so the replayer rebuilds the identical roster; a recording players share never holds a relay login.
		m_OpeningAuthorityPeerId = config.hostPeerId;
		std::vector<uint8_t> configBytes;
		NetLobbyError lobbyError;
		if (!NetLobbyProtocol::Encode({NetLobbyMatchConfig{NetMatchConfigUtil::WithoutRelay(config)}}, configBytes, &lobbyError)) {
			if (error) *error = "could not encode the replay config: " + lobbyError.message;
			Close();
			return false;
		}
		std::vector<uint8_t> header;
		AppendU32(header, c_Magic);
		AppendU16(header, c_Version);
		AppendU16(header, ControllerFrame::c_Version);
		AppendU32(header, static_cast<uint32_t>(configBytes.size()));
		header.insert(header.end(), configBytes.begin(), configBytes.end());
		// The segment block trails the config, so an ordinary version-6 recording is the version-5 bytes
		// plus one zero: nothing that reads the config by offset moves.
		header.push_back(segment ? 1 : 0);
		if (segment) {
			AppendString(header, segment->worldId);
			AppendU64(header, segment->tick);
			AppendU64(header, segment->round);
			AppendU64(header, segment->boot);
			AppendString(header, segment->worldDigest);
		}
		m_FramesWritten = 0;
		if (header.size() + 4 <= TelemetryBundle::c_MemberLimit) {
			m_DiagnosticBytes = header;
		} else m_DiagnosticTruncated = true;
		if (!m_Storage) {
			m_Storage = std::make_shared<StorageState>();
			try { std::thread(&NetMatchReplayWriter::WriteQueuedRecords, m_Storage).detach(); }
			catch (const std::exception& exception) { m_Storage.reset(); if (error) *error = exception.what(); return false; }
		}
		auto next = std::make_shared<WriteState>();
		next->path = path; next->header = std::move(header);
		next->pendingBytes = next->header.size();
		{
			std::lock_guard lock(m_Storage->mutex);
			if (next->header.size() > c_MaxRecordBytes + 8 - m_Storage->pendingBytes) {
				if (error) *error = "the replay storage queue reached its record-size limit";
				return false;
			}
			m_Storage->pendingBytes += next->header.size();
			m_Storage->segments.push_back(next);
			m_Writes = std::move(next);
		}
		m_Open = true;
		m_Storage->ready.notify_one();
		return true;
	}

	bool NetMatchReplayWriter::SetAgreedStart(const NetLockstepStart& start, std::string* error) {
		if (!IsOpen()) {
			if (error) *error = GetWriteError().empty() ? "replay writer is not open" : GetWriteError();
			return false;
		}
		if (!start.agreedStartRecord) {
			if (error) *error = "replay agreed start is not an agreed-start record";
			return false;
		}
		if (m_AgreedStartWritten) return m_AgreedStart && *m_AgreedStart == start;
		if (m_FramesWritten != 0) {
			if (error) *error = "replay agreed start must precede the first frame";
			return false;
		}
		NetLockstepError codecError;
		std::vector<uint8_t> wire;
		if (!NetLockstepCodec::Encode({start}, wire, &codecError)) {
			if (error) *error = "could not encode replay agreed start: " + codecError.message;
			return false;
		}
		std::vector<uint8_t> payload;
		AppendU32(payload, static_cast<uint32_t>(wire.size()));
		payload.insert(payload.end(), wire.begin(), wire.end());
		if (!WriteRecordPayload(payload, error)) return false;
		m_AgreedStart = start;
		m_AgreedStartWritten = true;
		return true;
	}

	bool NetMatchReplayWriter::WriteFrame(uint64_t frame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error) {
		return WriteFrame(frame, frames, commands, {}, error);
	}

	bool NetMatchReplayWriter::WriteFrame(uint64_t frame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, const std::vector<NetSoundObservation>& observations, std::string* error) {
		return WriteFrame(frame, frames, commands, observations, {}, error);
	}

	bool NetMatchReplayWriter::WriteFrame(uint64_t frame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, const std::vector<NetSoundObservation>& observations, const std::vector<NetValueObservation>& valueObservations, std::string* error, uint8_t authorityPeerId) {
		ReplayWorkTimer work{frame, "frame"};
		if (!IsOpen()) {
			if (error) *error = GetWriteError().empty() ? "replay writer is not open" : GetWriteError();
			return false;
		}
		NetLockstepFrame record;
		// The frame names its host; sender trailers retain each producer.
		record.senderPeerId = authorityPeerId != 0 ? authorityPeerId : m_OpeningAuthorityPeerId;
		record.targetFrame = frame;
		record.frames = frames;
		record.commands = commands;
		record.observations = observations;
		record.valueObservations = valueObservations;
		for (const NetGameCommand& command : commands) {
			if (command.senderPeerId == 0 || command.senderPeerId > NetLockstepCodec::c_MaxPeerCount) {
				if (error) *error = "invalid replay command sender";
				return false;
			}
		}
		for (const NetSoundObservation& observation : observations) {
			if (observation.senderPeerId == 0 || observation.senderPeerId > NetLockstepCodec::c_MaxPeerCount) {
				if (error) *error = "invalid replay observation sender";
				return false;
			}
		}
		for (const NetValueObservation& observation : valueObservations) {
			if (observation.senderPeerId == 0 || observation.senderPeerId > NetLockstepCodec::c_MaxPeerCount) {
				if (error) *error = "invalid replay value observation sender";
				return false;
			}
		}
		std::vector<uint8_t> wireBytes;
		const size_t bindingCount = static_cast<size_t>(std::count_if(commands.begin(), commands.end(), [](const NetGameCommand& command) {
			return std::holds_alternative<NetGamePlayerBindings>(command.payload);
		}));
		// A packet names each actor once and holds one sender's bound: a second input for one actor, from another sender, or the inputs
		// past that bound ride later packets in the order they apply.
		std::vector<std::vector<ControllerFrame>> frameRuns(1);
		for (const ControllerFrame& input: frames) {
			if (!frameRuns.back().empty() && (frameRuns.back().back().actorUniqueID == input.actorUniqueID || frameRuns.back().size() == NetLockstepCodec::c_MaxFramesPerPacket)) frameRuns.emplace_back();
			frameRuns.back().push_back(input);
		}
		if (bindingCount <= 1 && frameRuns.size() == 1) {
			if (!EncodeSenderPacket(record, wireBytes, error)) {
				return false;
			}
		} else {
			// Each packet carries a run of one sender's items, in the record's own order, so every list reads back in the order
			// it was applied and the sender trailers below name each item where it lands.
			std::vector<NetLockstepFrame> parts;
			const auto partFor = [&parts, frame](uint8_t sender) -> NetLockstepFrame& {
				if (parts.empty() || parts.back().senderPeerId != sender) {
					parts.emplace_back();
					parts.back().senderPeerId = sender;
					parts.back().targetFrame = frame;
				}
				return parts.back();
			};
			for (const NetGameCommand& command : commands) {
				partFor(command.senderPeerId).commands.push_back(command);
			}
			for (const NetSoundObservation& observation : observations) {
				partFor(observation.senderPeerId).observations.push_back(observation);
			}
			for (const NetValueObservation& observation : valueObservations) {
				partFor(observation.senderPeerId).valueObservations.push_back(observation);
			}
			if (parts.empty()) {
				partFor(1);
			}
			parts.front().frames = frameRuns.front();
			parts.front().senderPeerId = record.senderPeerId;
			const uint8_t inputSender = parts.front().senderPeerId;
			for (size_t run = 1; run < frameRuns.size(); ++run) {
				NetLockstepFrame& part = parts.emplace_back();
				part.senderPeerId = inputSender;
				part.targetFrame = frame;
				part.frames = frameRuns[run];
			}
			for (const NetLockstepFrame& part : parts) {
				std::vector<uint8_t> packet;
				if (!EncodeSenderPacket(part, packet, error)) {
					return false;
				}
				wireBytes.insert(wireBytes.end(), packet.begin(), packet.end());
			}
		}
		std::vector<uint8_t> bytes;
		bytes.reserve(4 + wireBytes.size() + commands.size() + observations.size() + valueObservations.size());
		AppendU32(bytes, static_cast<uint32_t>(wireBytes.size()));
		bytes.insert(bytes.end(), wireBytes.begin(), wireBytes.end());
		for (const NetGameCommand& command : commands) {
			bytes.push_back(command.senderPeerId);
		}
		for (const NetSoundObservation& observation : observations) {
			bytes.push_back(observation.senderPeerId);
		}
		for (const NetValueObservation& observation : valueObservations) {
			bytes.push_back(observation.senderPeerId);
		}
		if (!WriteRecordPayload(bytes, error)) return false;
		++m_FramesWritten;
		return true;
	}

	bool NetMatchReplayWriter::WriteRecordPayload(const std::vector<uint8_t>& payload, std::string* error) {
		if (!m_Open || payload.empty() || !m_Writes) {
			if (error) *error = "replay writer is not open";
			return false;
		}
		std::vector<uint8_t> lengthPrefix;
		AppendU32(lengthPrefix, static_cast<uint32_t>(payload.size()));
		AppendU32(lengthPrefix, ControllerFrameCodec::PayloadChecksum(payload));
		auto record = std::make_shared<WriteState::Record>();
		record->prefix = lengthPrefix;
		record->payload = payload;
		record->ordinal = m_FramesWritten + 1;
		record->beforeWrite = m_BeforeWriteForTest;
		{
			std::lock_guard lock(m_Storage->mutex);
			const size_t bytes = lengthPrefix.size() + payload.size();
			if (payload.size() > c_MaxRecordBytes || bytes > c_MaxRecordBytes + 8 - m_Storage->pendingBytes)
				m_Writes->error = "the replay storage queue reached its record-size limit";
			if (!m_Writes->error.empty()) {
				if (error) *error = m_Writes->error;
				return false;
			}
			m_Storage->pendingBytes += bytes;
			m_Writes->pendingBytes += bytes;
			m_Writes->records.push_back(record);
		}
		m_Storage->ready.notify_one();
		if (!m_DiagnosticTruncated) {
			ReplayWorkTimer work{m_FramesWritten + 1, "diagnostic", m_DiagnosticBytes.size()};
			if (m_DiagnosticBytes.size() + lengthPrefix.size() + payload.size() + 4 <= TelemetryBundle::c_MemberLimit) {
				m_DiagnosticBytes.insert(m_DiagnosticBytes.end(), lengthPrefix.begin(), lengthPrefix.end());
				m_DiagnosticBytes.insert(m_DiagnosticBytes.end(), payload.begin(), payload.end());
				++m_DiagnosticFrames;
			} else m_DiagnosticTruncated = true;
		}
		return true;
	}

	bool NetMatchReplayWriter::CopyDiagnosticReplay(std::string& bytes, bool& truncated) const {
		bytes.clear();
		truncated = m_DiagnosticTruncated;
		if (m_DiagnosticFrames == 0) return false;
		bytes.assign(reinterpret_cast<const char*>(m_DiagnosticBytes.data()), m_DiagnosticBytes.size());
		bytes.append(4, static_cast<char>(0xff));
		return true;
	}

	void NetMatchReplayWriter::Close() {
		m_Open = false;
		if (m_Writes && m_Storage) {
			{ std::lock_guard lock(m_Storage->mutex); m_Writes->stop = true; }
			m_Storage->ready.notify_one();
		}
		m_FramesWritten = 0;
		m_AgreedStart.reset();
		m_AgreedStartWritten = false;
	}

	bool NetMatchReplayReader::Open(const std::string& path, std::string* error) {
		Close();
		m_In.open(path, std::ios::binary);
		if (!m_In) {
			if (error) *error = "could not open replay file: " + path;
			return false;
		}
		uint32_t magic = 0;
		if (!ReadU32(m_In, magic) || magic != NetMatchReplayWriter::c_Magic) {
			if (error) *error = "not a replay file: " + path;
			Close();
			return false;
		}
		uint8_t versionBytes[2];
		if (!m_In.read(reinterpret_cast<char*>(versionBytes), 2)) {
			if (error) *error = "truncated replay header";
			Close();
			return false;
		}
		const uint16_t version = static_cast<uint16_t>(versionBytes[0]) | (static_cast<uint16_t>(versionBytes[1]) << 8);
		// Older versions retain their original frame and command semantics.
		if (version < 1 || version > NetMatchReplayWriter::c_Version) {
			if (error) *error = "unsupported replay version " + std::to_string(version);
			Close();
			return false;
		}
		m_Version = version;
		m_ControllerFrameVersion = ControllerFrame::c_LegacyVersion;
		if (version >= 3) {
			uint8_t frameVersionBytes[2];
			if (!m_In.read(reinterpret_cast<char*>(frameVersionBytes), 2)) {
				if (error) *error = "truncated replay header";
				Close();
				return false;
			}
			m_ControllerFrameVersion = static_cast<uint16_t>(frameVersionBytes[0]) | (static_cast<uint16_t>(frameVersionBytes[1]) << 8);
			if (!ControllerFrameCodec::IsSupportedVersion(m_ControllerFrameVersion)) {
				if (error) *error = "unsupported replay ControllerFrame version " + std::to_string(m_ControllerFrameVersion);
				Close();
				return false;
			}
		}
		uint32_t configLength = 0;
		if (!ReadU32(m_In, configLength) || configLength == 0 || configLength > (1U << 20)) {
			if (error) *error = "invalid replay config length";
			Close();
			return false;
		}
		std::vector<uint8_t> configBytes(configLength);
		if (!m_In.read(reinterpret_cast<char*>(configBytes.data()), static_cast<std::streamsize>(configLength))) {
			if (error) *error = "truncated replay config";
			Close();
			return false;
		}
		const NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(configBytes, NetLobbyDecodeOptions{true});
		const NetLobbyMatchConfig* configMessage = decoded.ok ? std::get_if<NetLobbyMatchConfig>(&decoded.message.payload) : nullptr;
		if (!configMessage) {
			if (error) *error = "could not decode the replay config" + (decoded.ok ? std::string() : ": " + decoded.error.message);
			Close();
			return false;
		}
		m_Config = configMessage->config;
		if (version >= 6) {
			uint8_t hasSegment = 0;
			if (!m_In.read(reinterpret_cast<char*>(&hasSegment), 1) || hasSegment > 1) {
				if (error) *error = "truncated replay header";
				Close();
				return false;
			}
			if (hasSegment == 1) {
				const size_t maxField = NetMatchReplayWriter::c_MaxSegmentFieldBytes;
				if (!ReadString(m_In, maxField, m_Segment.worldId) || !ReadU64(m_In, m_Segment.tick) ||
				    !ReadU64(m_In, m_Segment.round) || !ReadU64(m_In, m_Segment.boot) ||
				    !ReadString(m_In, maxField, m_Segment.worldDigest)) {
					if (error) *error = "truncated world segment header";
					Close();
					return false;
				}
				if (m_Segment.worldId.empty() || m_Segment.tick == 0) {
					if (error) *error = "invalid world segment header";
					Close();
					return false;
				}
				m_HasSegment = true;
			}
		}
		// The lookahead pins the start frame, so playback aligns to the recording's first tick.
		bool eof = false;
		if (!ReadFrameFromFile(m_Lookahead, eof, error)) {
			if (eof && m_HasSegment) { m_StartFrame = m_Segment.tick + 1; return true; }
			if (error && eof) *error = "replay file has no frames";
			Close();
			return false;
		}
		m_HasLookahead = true;
		m_StartFrame = m_Lookahead.targetFrame;
		return true;
	}

	bool NetMatchReplayReader::ReadFrame(NetLockstepFrame& outFrame, bool& outEof, std::string* error) {
		if (m_LastStatus == NetReplayReadStatus::CleanEnd) { outEof = true; return false; }
		if (m_HasLookahead) {
			outEof = false;
			outFrame = std::move(m_Lookahead);
			m_HasLookahead = false;
			m_LastStatus = NetReplayReadStatus::Frame;
			return true;
		}
		return ReadFrameFromFile(outFrame, outEof, error);
	}

	bool NetMatchReplayReader::ReadFrameFromFile(NetLockstepFrame& outFrame, bool& outEof, std::string* error) {
	while (true) {
		outEof = false;
		if (!m_In.is_open()) {
			if (error) *error = "replay reader is not open";
			return false;
		}
		uint32_t recordLength = 0;
		if (!ReadU32(m_In, recordLength)) {
			// A version-2 file ends with the marker below, so raw EOF here means the record stream was
			// cut off mid-write. Version-1 files have no marker, so raw EOF is their clean end.
			if (m_Version >= 2) {
				m_LastStatus = NetReplayReadStatus::Truncated;
				if (error) *error = "replay ended without its end marker (truncated)";
				return false;
			}
			m_LastStatus = NetReplayReadStatus::CleanEnd;
			outEof = true;
			return false;
		}
		if (recordLength == NetMatchReplayWriter::c_EndMarker) {
			m_LastStatus = NetReplayReadStatus::CleanEnd;
			outEof = true;
			return false;
		}
		m_LastStatus = NetReplayReadStatus::Corrupt;
		if (recordLength == 0 || recordLength > NetMatchReplayWriter::c_MaxRecordBytes) {
			if (error) *error = "invalid replay record length";
			return false;
		}
		uint32_t checksum = 0;
		if (m_Version >= 4 && !ReadU32(m_In, checksum)) {
			m_LastStatus = NetReplayReadStatus::Truncated;
			if (error) *error = "truncated replay record";
			return false;
		}
		std::vector<uint8_t> bytes(recordLength);
		if (!m_In.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(recordLength))) {
			m_LastStatus = NetReplayReadStatus::Truncated;
			if (error) *error = "truncated replay record";
			return false;
		}
		if (m_Version >= 4 && ControllerFrameCodec::PayloadChecksum(bytes) != checksum) {
			if (error) *error = "replay record checksum mismatch";
			return false;
		}
		size_t wireOffset = 0;
		size_t wireLength = bytes.size();
		if (m_Version >= 5) {
			if (bytes.size() < 4) {
				if (error) *error = "truncated replay frame envelope";
				return false;
			}
			wireOffset = 4;
			wireLength = ReadU32(bytes.data());
			if (wireLength == 0 || wireLength > bytes.size() - wireOffset) {
				if (error) *error = "invalid replay wire frame length";
				return false;
			}
		}
		size_t consumed = 0;
		bool haveFrame = false;
		std::optional<NetLockstepStart> agreedStart;
		while (consumed < wireLength) {
			const size_t packetSize = LockstepPacketSize(bytes.data() + wireOffset + consumed, wireLength - consumed);
			if (packetSize == 0) {
				if (error) *error = "invalid replay wire frame length";
				return false;
			}
			const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes.data() + wireOffset + consumed, packetSize, m_ControllerFrameVersion);
			if (!decoded.ok) {
				if (error) *error = "could not decode a replay record: " + decoded.error.message;
				return false;
			}
			if (const auto* start = std::get_if<NetLockstepStart>(&decoded.packet.payload)) {
				if (!start->agreedStartRecord || agreedStart) {
					if (error) *error = "replay metadata is not a single agreed-start record";
					return false;
				}
				agreedStart = *start;
			} else if (const auto* frame = std::get_if<NetLockstepFrame>(&decoded.packet.payload)) {
				if (!haveFrame) {
					outFrame = *frame;
					haveFrame = true;
				} else {
					outFrame.frames.insert(outFrame.frames.end(), frame->frames.begin(), frame->frames.end());
					outFrame.commands.insert(outFrame.commands.end(), frame->commands.begin(), frame->commands.end());
					outFrame.observations.insert(outFrame.observations.end(), frame->observations.begin(), frame->observations.end());
					outFrame.valueObservations.insert(outFrame.valueObservations.end(), frame->valueObservations.begin(), frame->valueObservations.end());
				}
			} else {
				if (error) *error = "replay record is not a frame or agreed-start metadata";
				return false;
			}
			consumed += packetSize;
		}
		if (!haveFrame) {
			if (!agreedStart) {
				if (error) *error = "replay record contains no frame";
				return false;
			}
			m_AgreedStart = *agreedStart;
			continue;
		}
		if (m_Version >= 5) {
			const size_t senderOffset = wireOffset + wireLength;
			if (bytes.size() - senderOffset != outFrame.commands.size() + outFrame.observations.size() + outFrame.valueObservations.size()) {
				if (error) *error = "replay command sender count mismatch";
				return false;
			}
			for (size_t i = 0; i < outFrame.commands.size(); ++i) {
				const uint8_t sender = bytes[senderOffset + i];
				if (sender == 0 || sender > NetLockstepCodec::c_MaxPeerCount) {
					if (error) *error = "invalid replay command sender";
					return false;
				}
				outFrame.commands[i].senderPeerId = sender;
			}
			const size_t observationOffset = senderOffset + outFrame.commands.size();
			for (size_t i = 0; i < outFrame.observations.size(); ++i) {
				const uint8_t sender = bytes[observationOffset + i];
				if (sender == 0 || sender > NetLockstepCodec::c_MaxPeerCount) {
					if (error) *error = "invalid replay observation sender";
					return false;
				}
				outFrame.observations[i].senderPeerId = sender;
			}
			const size_t valueOffset = observationOffset + outFrame.observations.size();
			for (size_t i = 0; i < outFrame.valueObservations.size(); ++i) {
				const uint8_t sender = bytes[valueOffset + i];
				if (sender == 0 || sender > NetLockstepCodec::c_MaxPeerCount) {
					if (error) *error = "invalid replay value observation sender";
					return false;
				}
				outFrame.valueObservations[i].senderPeerId = sender;
			}
		}
		if (m_Version >= 8) {
			if (outFrame.senderPeerId == 0 || outFrame.senderPeerId > m_Config.peerCount) {
				if (error) *error = "invalid replay frame authority";
				return false;
			}
			outFrame.replayAuthorityPeerId = outFrame.senderPeerId;
		}
		m_LastStatus = NetReplayReadStatus::Frame;
		return true;
	}
	}

	std::string NetReplayVerifyReport::ToJson() const {
		std::string json = "{";
		json += "\"ok\":" + std::string(ok ? "true" : "false");
		json += ",\"version\":" + std::to_string(version);
		json += ",\"controller_frame_version\":" + std::to_string(controllerFrameVersion);
		json += ",\"frames\":" + std::to_string(frames);
		json += ",\"first_frame\":" + std::to_string(firstFrame);
		json += ",\"last_frame\":" + std::to_string(lastFrame);
		json += ",\"gaps\":" + std::to_string(gaps);
		json += ",\"end_marker\":" + std::string(endMarker ? "true" : "false");
		json += ",\"truncated\":" + std::string(truncated ? "true" : "false");
		json += ",\"corrupt\":" + std::string(corrupt ? "true" : "false");
		json += ",\"segment\":" + std::string(segment ? "true" : "false");
		if (segment) {
			json += ",\"world_id\":\"" + segmentHeader.worldId + "\"";
			json += ",\"segment_tick\":" + std::to_string(segmentHeader.tick);
			json += ",\"segment_round\":" + std::to_string(segmentHeader.round);
			json += ",\"segment_boot\":" + std::to_string(segmentHeader.boot);
			json += ",\"world_digest\":\"" + segmentHeader.worldDigest + "\"";
		}
		std::string escaped;
		for (const char c: error) {
			if (c == '"' || c == '\\') {
				escaped += '\\';
			}
			escaped += (c == '\n') ? ' ' : c;
		}
		json += ",\"error\":\"" + escaped + "\"";
		json += "}";
		return json;
	}

	bool NetMatchReplayReader::Verify(const std::string& path, NetReplayVerifyReport& outReport) {
		outReport = NetReplayVerifyReport{};
		NetMatchReplayReader reader;
		std::string error;
		if (!reader.Open(path, &error)) {
			outReport.error = error;
			outReport.corrupt = true;
			return false;
		}
		outReport.version = reader.GetVersion();
		outReport.controllerFrameVersion = reader.GetControllerFrameVersion();
		outReport.segment = reader.HasWorldSegment();
		outReport.segmentHeader = reader.GetWorldSegment();
		uint64_t previousFrame = 0;
		while (true) {
			NetLockstepFrame record;
			bool eof = false;
			error.clear();
			if (reader.ReadFrame(record, eof, &error)) {
				if (outReport.frames == 0) {
					outReport.firstFrame = record.targetFrame;
				} else if (record.targetFrame != previousFrame + 1) {
					++outReport.gaps;
				}
				previousFrame = record.targetFrame;
				outReport.lastFrame = record.targetFrame;
				++outReport.frames;
				continue;
			}
			switch (reader.GetLastReadStatus()) {
				case NetReplayReadStatus::CleanEnd:
					// Version-1 files have no marker; their raw EOF is the clean end they know.
					outReport.endMarker = true;
					break;
				case NetReplayReadStatus::Truncated:
					outReport.truncated = true;
					outReport.error = error;
					break;
				default:
					outReport.corrupt = true;
					outReport.error = error.empty() ? "replay record failed to decode" : error;
					break;
			}
			break;
		}
		if (outReport.gaps > 0) {
			outReport.corrupt = true;
		}
		outReport.ok = outReport.endMarker && !outReport.truncated && !outReport.corrupt && outReport.frames > 0 && outReport.gaps == 0;
		if (!outReport.ok && outReport.error.empty()) {
			outReport.error = outReport.frames == 0 ? "replay has no frames" : (outReport.gaps > 0 ? "replay has tick gaps" : "replay is incomplete");
		}
		return outReport.ok;
	}

	void NetMatchReplayReader::Close() {
		if (m_In.is_open()) {
			m_In.close();
		}
		m_Config = {};
		m_Lookahead = {};
		m_HasLookahead = false;
		m_StartFrame = 0;
		m_Version = 0;
		m_ControllerFrameVersion = 0;
		m_HasSegment = false;
		m_Segment = {};
		m_AgreedStart.reset();
	}

} // namespace RTE
