#include "NetMatchReplay.h"

#include "NetLobbyProtocol.h"

namespace RTE {

	namespace {
		void AppendU16(std::vector<uint8_t>& out, uint16_t value) {
			out.push_back(static_cast<uint8_t>(value & 0xFFU));
			out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
		}

		void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
			for (int shift = 0; shift < 32; shift += 8) {
				out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
			}
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
	} // namespace

	bool NetMatchReplayWriter::Open(const std::string& path, const NetMatchConfig& config, std::string* error) {
		Close();
		m_Out.open(path, std::ios::binary | std::ios::trunc);
		if (!m_Out) {
			if (error) *error = "could not open replay file for writing: " + path;
			return false;
		}
		// The synced config rides the lobby codec, so the replayer rebuilds the identical roster.
		std::vector<uint8_t> configBytes;
		NetLobbyError lobbyError;
		if (!NetLobbyProtocol::Encode({NetLobbyMatchConfig{config}}, configBytes, &lobbyError)) {
			if (error) *error = "could not encode the replay config: " + lobbyError.message;
			Close();
			return false;
		}
		std::vector<uint8_t> header;
		AppendU32(header, c_Magic);
		AppendU16(header, c_Version);
		AppendU16(header, ControllerFrame::c_Version);
		AppendU32(header, static_cast<uint32_t>(configBytes.size()));
		m_Out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
		m_Out.write(reinterpret_cast<const char*>(configBytes.data()), static_cast<std::streamsize>(configBytes.size()));
		if (!m_Out) {
			if (error) *error = "could not write the replay header";
			Close();
			return false;
		}
		m_FramesWritten = 0;
		return true;
	}

	bool NetMatchReplayWriter::WriteFrame(uint64_t frame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error) {
		if (!m_Out.is_open()) {
			if (error) *error = "replay writer is not open";
			return false;
		}
		NetLockstepFrame record;
		// The wire codec authenticates one sender; a committed tick can contain several.
		record.senderPeerId = 1;
		record.targetFrame = frame;
		record.frames = frames;
		record.commands = commands;
		for (const NetGameCommand& command : commands) {
			if (command.senderPeerId == 0 || command.senderPeerId > NetLockstepCodec::c_MaxPeerCount) {
				if (error) *error = "invalid replay command sender";
				return false;
			}
		}
		std::vector<uint8_t> wireBytes;
		NetLockstepError codecError;
		if (!NetLockstepCodec::Encode({record}, wireBytes, &codecError)) {
			if (error) *error = "could not encode a replay frame: " + codecError.message;
			return false;
		}
		std::vector<uint8_t> bytes;
		bytes.reserve(4 + wireBytes.size() + commands.size());
		AppendU32(bytes, static_cast<uint32_t>(wireBytes.size()));
		bytes.insert(bytes.end(), wireBytes.begin(), wireBytes.end());
		for (const NetGameCommand& command : commands) {
			bytes.push_back(command.senderPeerId);
		}
		std::vector<uint8_t> lengthPrefix;
		AppendU32(lengthPrefix, static_cast<uint32_t>(bytes.size()));
		AppendU32(lengthPrefix, ControllerFrameCodec::PayloadChecksum(bytes));
		m_Out.write(reinterpret_cast<const char*>(lengthPrefix.data()), static_cast<std::streamsize>(lengthPrefix.size()));
		m_Out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!m_Out) {
			if (error) *error = "could not write a replay frame";
			return false;
		}
		++m_FramesWritten;
		return true;
	}

	void NetMatchReplayWriter::Close() {
		if (m_Out.is_open()) {
			// A recording with frames gets an end marker so playback tells a clean finish from a
			// truncation; a header-only or already-failed stream is left as is.
			if (m_FramesWritten > 0 && m_Out.good()) {
				std::vector<uint8_t> endMarker;
				AppendU32(endMarker, c_EndMarker);
				m_Out.write(reinterpret_cast<const char*>(endMarker.data()), static_cast<std::streamsize>(endMarker.size()));
			}
			m_Out.close();
		}
		m_FramesWritten = 0;
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
		// The recorded MatchConfig layout is unchanged from lobby version 2.
		if (configBytes.size() >= NetLobbyProtocol::c_HeaderBytes && configBytes[4] == 2 && configBytes[5] == 0 &&
		    configBytes[8] == static_cast<uint8_t>(NetLobbyMessageType::MatchConfig) && configBytes[9] == 0) {
			configBytes[4] = static_cast<uint8_t>(NetLobbyProtocol::c_Version);
			configBytes[5] = static_cast<uint8_t>(NetLobbyProtocol::c_Version >> 8);
		}
		const NetLobbyDecodeResult decoded = NetLobbyProtocol::Decode(configBytes);
		const NetLobbyMatchConfig* configMessage = decoded.ok ? std::get_if<NetLobbyMatchConfig>(&decoded.message.payload) : nullptr;
		if (!configMessage) {
			if (error) *error = "could not decode the replay config" + (decoded.ok ? std::string() : ": " + decoded.error.message);
			Close();
			return false;
		}
		m_Config = configMessage->config;
		// The lookahead pins the start frame, so playback aligns to the recording's first tick.
		bool eof = false;
		if (!ReadFrameFromFile(m_Lookahead, eof, error)) {
			if (error && eof) *error = "replay file has no frames";
			Close();
			return false;
		}
		m_HasLookahead = true;
		m_StartFrame = m_Lookahead.targetFrame;
		return true;
	}

	bool NetMatchReplayReader::ReadFrame(NetLockstepFrame& outFrame, bool& outEof, std::string* error) {
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
		if (recordLength == 0 || recordLength > (1U << 24)) {
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
		const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes.data() + wireOffset, wireLength, m_ControllerFrameVersion);
		if (!decoded.ok) {
			if (error) *error = "could not decode a replay record: " + decoded.error.message;
			return false;
		}
		const NetLockstepFrame* frame = std::get_if<NetLockstepFrame>(&decoded.packet.payload);
		if (!frame) {
			if (error) *error = "replay record is not a frame";
			return false;
		}
		outFrame = *frame;
		if (m_Version >= 5) {
			const size_t senderOffset = wireOffset + wireLength;
			if (bytes.size() - senderOffset != outFrame.commands.size()) {
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
		}
		m_LastStatus = NetReplayReadStatus::Frame;
		return true;
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
	}

} // namespace RTE
