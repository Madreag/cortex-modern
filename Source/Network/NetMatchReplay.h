#pragma once

#include "NetLockstep.h"
#include "NetMatchConfig.h"

#include <cstdint>
#include <fstream>
#include <optional>
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

	/// The world checkpoint a segment recording stands on. A persistent world's recording is cut at every
	/// checkpoint, so each segment carries the frames after one archive and names that archive here.
	struct NetWorldSegmentHeader {
		std::string worldId;
		uint64_t tick = 0;  //!< The checkpoint's committed tick; the segment's first frame is tick + 1.
		uint64_t round = 0;
		uint64_t boot = 0;
		std::string worldDigest; //!< The checkpoint archive's world-structure hash, as the descriptor stores it.
		bool operator==(const NetWorldSegmentHeader&) const = default;
	};

	/// The whole-file integrity scan (-net-replay-verify): every record decodes, the end marker is present, no tick gaps.
	struct NetReplayVerifyReport {
		bool ok = false;
		std::string error;
		uint16_t version = 0;
		uint16_t controllerFrameVersion = 0;
		uint64_t frames = 0;
		uint64_t firstFrame = 0;
		uint64_t lastFrame = 0;
		uint64_t gaps = 0;
		bool endMarker = false;
		bool truncated = false;
		bool corrupt = false;
		bool segment = false; //!< Whether the header named a world checkpoint.
		NetWorldSegmentHeader segmentHeader;
		std::string ToJson() const;
	};

	class NetMatchReplayWriter {
	public:
		static constexpr uint32_t c_Magic = 0x50524343U; // "CCRP"
		// Version 6 carries the world checkpoint a segment stands on; version 5 preserved each committed
		// command's sender beside the checksummed wire frame.
		static constexpr uint16_t c_Version = 6;
		/// The longest world id and digest a segment header may carry; both are bounded strings already.
		static constexpr size_t c_MaxSegmentFieldBytes = 64;
		// A length prefix above the record cap; the writer appends it as the last record so playback
		// tells a clean end from a mid-write crash. Version-1 files have no marker.
		static constexpr uint32_t c_EndMarker = 0xFFFFFFFFU;

		bool Open(const std::string& path, const NetMatchConfig& config, std::string* error = nullptr);
		/// A world segment: the header names the checkpoint the records stand on. A null segment writes
		/// an ordinary recording, which boots the preset instead of a world snapshot.
		bool Open(const std::string& path, const NetMatchConfig& config, const NetWorldSegmentHeader* segment, std::string* error);
		bool WriteFrame(uint64_t frame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, std::string* error = nullptr);
		/// A committed tick with every peer's sound observations; their senders trail the record like the commands'.
		bool WriteFrame(uint64_t frame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, const std::vector<NetSoundObservation>& observations, std::string* error);
		bool WriteFrame(uint64_t frame, const std::vector<ControllerFrame>& frames, const std::vector<NetGameCommand>& commands, const std::vector<NetSoundObservation>& observations, const std::vector<NetValueObservation>& valueObservations, std::string* error);
		/// Stores the host's one-time agreed-start boundary before the first recorded tick.
		bool SetAgreedStart(const NetLockstepStart& start, std::string* error = nullptr);
		bool HasAgreedStart() const { return m_AgreedStart.has_value(); }
		void Close();
		bool IsOpen() const { return m_Out.is_open(); }
		uint64_t GetFramesWritten() const { return m_FramesWritten; }
		/// Copies complete recorded ticks from memory, with an end marker; call only at a tick boundary.
		bool CopyDiagnosticReplay(std::string& bytes, bool& truncated) const;

	private:
		bool WriteRecordPayload(const std::vector<uint8_t>& payload, std::string* error);
		std::ofstream m_Out;
		uint64_t m_FramesWritten = 0;
		std::vector<uint8_t> m_DiagnosticBytes;
		uint64_t m_DiagnosticFrames = 0;
		bool m_DiagnosticTruncated = false;
		std::optional<NetLockstepStart> m_AgreedStart;
		bool m_AgreedStartWritten = false;
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
		/// Whether this recording is a world segment; a version-5 or older file never is.
		bool HasWorldSegment() const { return m_HasSegment; }
		const NetWorldSegmentHeader& GetWorldSegment() const { return m_Segment; }
		const std::optional<NetLockstepStart>& GetAgreedStart() const { return m_AgreedStart; }
		uint16_t GetVersion() const { return m_Version; }
		/// The ControllerFrame version the records decode with; pre-version-3 files carry the legacy frame.
		uint16_t GetControllerFrameVersion() const { return m_ControllerFrameVersion; }
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
		uint16_t m_ControllerFrameVersion = 0;
		bool m_HasSegment = false;
		NetWorldSegmentHeader m_Segment;
		std::optional<NetLockstepStart> m_AgreedStart;
		NetReplayReadStatus m_LastStatus = NetReplayReadStatus::None;
	};

} // namespace RTE
