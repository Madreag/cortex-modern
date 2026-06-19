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

	struct NetIdentityBuildOptions {
		std::string buildId = "unknown";
		std::string sessionRulesTag = "p2-session-rules-unset";
		bool includeUserdataModules = false;
	};

	class NetIdentity {
	public:
		static bool BuildCurrentManifest(NetIdentityManifest& outManifest, std::string* error = nullptr, NetIdentityBuildOptions options = {});
		static bool WriteManifestJson(const NetIdentityManifest& manifest, const std::string& path, std::string* error = nullptr);
		static bool DumpCurrentManifestJson(const std::string& path, std::string* error = nullptr, NetIdentityManifest* outManifest = nullptr);

		static std::optional<NetIdentityMismatch> Compare(const NetIdentityManifest& expected, const NetIdentityManifest& actual, bool rejectUserdataModules = true);

		static NetHash32 HashCanonicalText(const std::string& domain, const std::vector<std::pair<std::string, std::string>>& fields);
		static std::string HashHex(const NetHash32& hash);
		static std::string ShortHashHex(const NetHash32& hash);
		static std::string NormalizeRelativePathForHash(const std::string& path, std::string* error = nullptr);
	};

} // namespace RTE
