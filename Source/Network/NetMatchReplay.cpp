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

		bool ReadU32(std::ifstream& in, uint32_t& outValue) {
			uint8_t bytes[4];
			if (!in.read(reinterpret_cast<char*>(bytes), 4)) {
				return false;
			}
			outValue = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
			           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
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
		// The codec wants a valid peer id; a replay record's sender is meaningless (commands carry
		// their own).
		record.senderPeerId = 1;
		record.targetFrame = frame;
		record.frames = frames;
		record.commands = commands;
		std::vector<uint8_t> bytes;
		NetLockstepError codecError;
		if (!NetLockstepCodec::Encode({record}, bytes, &codecError)) {
			if (error) *error = "could not encode a replay frame: " + codecError.message;
			return false;
		}
		std::vector<uint8_t> lengthPrefix;
		AppendU32(lengthPrefix, static_cast<uint32_t>(bytes.size()));
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
		// Version 1 (no end marker) still reads; version 2 adds the truncation-detecting marker.
		if (version != 1 && version != NetMatchReplayWriter::c_Version) {
			if (error) *error = "unsupported replay version " + std::to_string(version);
			Close();
			return false;
		}
		m_Version = version;
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
				if (error) *error = "replay ended without its end marker (truncated)";
				return false;
			}
			outEof = true;
			return false;
		}
		if (recordLength == NetMatchReplayWriter::c_EndMarker) {
			outEof = true;
			return false;
		}
		if (recordLength == 0 || recordLength > (1U << 24)) {
			if (error) *error = "invalid replay record length";
			return false;
		}
		std::vector<uint8_t> bytes(recordLength);
		if (!m_In.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(recordLength))) {
			if (error) *error = "truncated replay record";
			return false;
		}
		const NetLockstepDecodeResult decoded = NetLockstepCodec::Decode(bytes);
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
		return true;
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
	}

} // namespace RTE
