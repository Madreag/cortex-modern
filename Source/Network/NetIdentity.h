#pragma once

#include "NetProtocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RTE {

	struct NetIdentityDeterministicConfig {
		std::string gameVersion;
		uint16_t networkProtocolVersion = 0;
		uint16_t controllerFrameVersion = 0;
		uint16_t controllerFrameEncodedSize = 0;
		std::string deltaTimeBits;
		int aiUpdateInterval = 0;
		int pathfinderGridNodeSize = 0;
		int recommendedMoidCount = 0;
		bool particleSettling = false;
		bool moSubtraction = false;
		int numLuaStates = 0;
		int numLuaStatesOverride = -1;
		std::string selectedModule;
		bool scenarioTestModuleLoaded = false;
		uint16_t lockstepCodecVersion = 0;
		uint16_t matchConfigVersion = 0;
		uint16_t lobbyProtocolVersion = 0;
		std::string enabledGlobalScripts; //!< Sim-mutating global scripts run off per-machine Settings; a mismatch must reject at join.

		bool operator==(const NetIdentityDeterministicConfig&) const = default;
	};

	struct NetIdentityModuleEntry {
		int index = 0;
		std::string fileName;
		std::string friendlyName;
		std::string author;
		int version = 0;
		bool official = false;
		bool userdata = false;
		std::string root;
		uint64_t fileCount = 0;
		uint64_t totalBytes = 0;
		NetHash32 contentHash{};

		bool operator==(const NetIdentityModuleEntry&) const = default;
	};

	struct NetIdentityManifest {
		int schema = 1;
		std::string gameVersion;
		uint16_t networkProtocolVersion = 0;
		uint16_t controllerFrameVersion = 0;
		uint16_t controllerFrameEncodedSize = 0;
		std::string buildId;
		std::string platform;
		NetIdentityDeterministicConfig deterministicConfig;
		std::vector<NetIdentityModuleEntry> modules;
		bool hasUserdataModules = false;
		NetHash32 deterministicConfigHash{};
		NetHash32 moduleManifestHash{};
		NetHash32 sessionRulesHash{};
		NetHash32 sessionIdentityHash{};
		uint64_t hashDurationMs = 0;
		std::vector<std::string> warnings;
	};

	struct NetIdentityMismatch {
		std::string key;
		NetRejectReason rejectReason = NetRejectReason::InternalError;
		std::string expectedShortValue;
		std::string actualShortValue;
		std::string moduleName;
		std::string filePath;
		std::string summary;
	};

	/// A module both peers loaded whose content or version differs. The versions are named from the
	/// joiner's side, so "local" is always the host's and "remote" always the joiner's.
	struct NetModuleVersionDifference {
		std::string fileName;
		uint32_t localVersion = 0;
		uint32_t remoteVersion = 0;
		bool contentDiffers = false;

		bool operator==(const NetModuleVersionDifference&) const = default;
	};

	struct NetModuleDiff {
		std::vector<std::string> missingOnRemote;
		std::vector<std::string> extraOnRemote;
		std::vector<NetModuleVersionDifference> differing;

		bool Empty() const { return missingOnRemote.empty() && extraOnRemote.empty() && differing.empty(); }
		size_t Count() const { return missingOnRemote.size() + extraOnRemote.size() + differing.size(); }
	};

	struct NetIdentityBuildOptions {
		std::string buildId = "unknown";
		std::string sessionRulesTag = "p2-session-rules-unset";
		bool includeUserdataModules = false;
	};

	class NetIdentity {
	public:
		static bool BuildCurrentManifest(NetIdentityManifest& outManifest, std::string* error = nullptr, NetIdentityBuildOptions options = {});

		/// Reads everything the manifest needs from the live managers, and nothing from disk. Cheap, and
		/// the only phase that has to run on the thread that owns those managers.
		static bool CaptureManifestInputs(NetIdentityManifest& outManifest, std::string* error = nullptr, NetIdentityBuildOptions options = {});

		/// Hashes the captured modules' files and fills the manifest's hashes. Touches no manager, so a
		/// worker thread can do this work while the game thread keeps its frame budget.
		static bool CompleteManifestFromInputs(NetIdentityManifest& manifest, std::string* error = nullptr, NetIdentityBuildOptions options = {});
		static bool WriteManifestJson(const NetIdentityManifest& manifest, const std::string& path, std::string* error = nullptr);
		static bool DumpCurrentManifestJson(const std::string& path, std::string* error = nullptr, NetIdentityManifest* outManifest = nullptr);

		static std::optional<NetIdentityMismatch> Compare(const NetIdentityManifest& expected, const NetIdentityManifest& actual, bool rejectUserdataModules = true);

		/// The loaded modules as the diagnostic digest wire carries them, sorted by file name and cut
		/// to the entry and byte caps. Diagnostic only - admission still decides on the full hashes.
		static std::vector<NetModuleDigestEntry> BuildModuleDigests(const std::vector<NetIdentityModuleEntry>& modules, size_t maxEntries, bool* outTruncated = nullptr);
		/// Diffs two digest lists by file name, never by load order, so one extra module does not make
		/// every module after it look different.
		static NetModuleDiff DiffModules(const std::vector<NetModuleDigestEntry>& local, const std::vector<NetModuleDigestEntry>& remote);
		/// One joiner-facing sentence naming what to install, remove or update, or an empty string when
		/// the digests named no difference at all.
		static std::string DescribeModuleDiff(const NetModuleDiff& diff, size_t maxNamedPerGroup = 6, bool truncated = false);

		static NetHash32 HashCanonicalText(const std::string& domain, const std::vector<std::pair<std::string, std::string>>& fields);
		static NetHash32 HashDeterministicConfig(const NetIdentityDeterministicConfig& config);
		static NetHash32 HashModuleManifest(const std::vector<NetIdentityModuleEntry>& modules);
		static NetHash32 HashSessionIdentity(const NetIdentityManifest& manifest);
		static std::string HashHex(const NetHash32& hash);
		static std::string ShortHashHex(const NetHash32& hash);
		static std::string NormalizeRelativePathForHash(const std::string& path, std::string* error = nullptr);
	};

} // namespace RTE
