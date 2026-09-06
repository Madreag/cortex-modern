#pragma once

#include "NetLockstep.h"
#include "NetMatchConfig.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace RTE {

	/// A recorded lockstep match: the synced config, then every committed frame (all peers' controller
	/// frames + game commands per tick). Replaying feeds the records through the standard lockstep
	/// apply path, so a deterministic sim reproduces the match bit-for-bit.
	/// How the last read on a replay stream ended. Playback classifies its outcome from this, never from text.
	enum class NetReplayReadStatus {
		None,
		Frame,
		CleanEnd,
		Truncated,
		Corrupt,
	};

	/// The whole-file integrity scan (-net-replay-verify): every record decodes, the end marker is present, no tick gaps.
	struct NetReplayVerifyReport {
		bool ok = false;
		std::string error;
		uint16_t version = 0;
		uint64_t frames = 0;
		uint64_t firstFrame = 0;
		uint64_t lastFrame = 0;
		uint64_t gaps = 0;
		bool endMarker = false;
		bool truncated = false;
		bool corrupt = false;
		std::string ToJson() const;
	};

	class NetMatchReplayWriter {
	public:
		static constexpr uint32_t c_Magic = 0x50524343U; // "CCRP"
		static constexpr uint16_t c_Version = 2;
		// A length prefix above the record cap; the writer appends it as the last record so playback
		// tells a clean end from a mid-write crash. Version-1 files have no marker.
		static constexpr uint32_t c_EndMarker = 0xFFFFFFFFU;

		bool Open(const std::string& path, const NetMatchConfig& config, std::string* error = nullptr);
		bool WriteFrame(uint64_t frame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error = nullptr);
		void Close();
		bool IsOpen() const { return m_Out.is_open(); }
		uint64_t GetFramesWritten() const { return m_FramesWritten; }

	private:
		std::ofstream m_Out;
		uint64_t m_FramesWritten = 0;
	};

	class NetMatchReplayReader {
	public:
		bool Open(const std::string& path, std::string* error = nullptr);
		const NetMatchConfig& GetConfig() const { return m_Config; }
		/// The first record's frame number — the playback coordinator starts there, so recordings
		/// replay regardless of which sim tick the recorder's match began on.
		uint64_t GetStartFrame() const { return m_StartFrame; }
		/// Reads the next record. Returns false with outEof=true at the clean end of the file.
		bool ReadFrame(NetLockstepFrame& outFrame, bool& outEof, std::string* error = nullptr);
		NetReplayReadStatus GetLastReadStatus() const { return m_LastStatus; }
		uint16_t GetVersion() const { return m_Version; }
		static bool Verify(const std::string& path, NetReplayVerifyReport& outReport);
		void Close();
		bool IsOpen() const { return m_In.is_open(); }

	private:
		bool ReadFrameFromFile(NetLockstepFrame& outFrame, bool& outEof, std::string* error);

		std::ifstream m_In;
		NetMatchConfig m_Config;
		NetLockstepFrame m_Lookahead;
		bool m_HasLookahead = false;
		uint64_t m_StartFrame = 0;
		uint16_t m_Version = 0; //!< Version-2 files carry an end marker, so raw EOF without it is a truncation.
		NetReplayReadStatus m_LastStatus = NetReplayReadStatus::None;
	};

} // namespace RTE
