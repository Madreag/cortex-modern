#pragma once

#include <cstddef>
#include <string>

namespace RTE {

	/// Writes diagnostics from owned data; the worker never reads simulation objects.
	class TelemetryBundle {
	public:
		static constexpr size_t c_MemberLimit = 8 * 1024 * 1024;
		static constexpr size_t c_LogTailLimit = 512 * 1024;

		struct Snapshot {
			std::string consoleTail;
			std::string joinIdentity;
			std::string desyncHeal;
			std::string replay;
			std::string replayReason = "no recording";
			bool replayTruncated = false;
		};

		/// Installs a bounded network-log mirror and starts the writer before any match.
		static void Initialize(const std::string& gpu);
		/// Queues immutable copies captured at a completed tick; false while a bundle is pending.
		static bool Request(Snapshot snapshot);
		/// Joins the writer at process shutdown and restores the original log streams.
		static void Finish();
	};
}
