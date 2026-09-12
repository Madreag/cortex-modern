#include "NetIdentitySelfTest.h"

#include "NetIdentity.h"

#include <iostream>
#include <string>
#include <vector>

namespace RTE {

	namespace {
		NetHash32 MakeHash(uint8_t seed) {
			NetHash32 hash{};
			for (size_t i = 0; i < hash.size(); ++i) {
				hash[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
			}
			return hash;
		}

		NetIdentityManifest MakeManifest() {
			NetIdentityManifest manifest;
			manifest.gameVersion = "7.0.0";
			manifest.networkProtocolVersion = 1;
			manifest.controllerFrameVersion = 5;
			manifest.controllerFrameEncodedSize = 80;
			manifest.buildId = "selftest";
			manifest.platform = "test";
			manifest.deterministicConfigHash = MakeHash(1);
			manifest.moduleManifestHash = MakeHash(33);
			manifest.sessionRulesHash = MakeHash(65);
			manifest.sessionIdentityHash = MakeHash(97);

			NetIdentityModuleEntry base;
			base.index = 0;
			base.fileName = "Base.rte";
			base.friendlyName = "Base";
			base.author = "Data Realms";
			base.version = 1;
			base.official = true;
			base.userdata = false;
			base.root = "Data/Base.rte";
			base.fileCount = 2;
			base.totalBytes = 42;
			base.contentHash = MakeHash(129);
			manifest.modules.push_back(base);

			NetIdentityModuleEntry mod = base;
			mod.index = 1;
			mod.fileName = "Example.rte";
			mod.friendlyName = "Example";
			mod.official = false;
			mod.root = "Mods/Example.rte";
			mod.contentHash = MakeHash(161);
			manifest.modules.push_back(mod);
			return manifest;
		}

		bool ExpectMismatchKey(const NetIdentityManifest& expected, const NetIdentityManifest& actual, const std::string& key, std::string* error) {
			const std::optional<NetIdentityMismatch> mismatch = NetIdentity::Compare(expected, actual);
			if (!mismatch) {
				*error = "expected mismatch key " + key + " but manifests compared equal";
				return false;
			}
			if (mismatch->key != key) {
				*error = "expected mismatch key " + key + " but got " + mismatch->key;
				return false;
			}
			if (mismatch->summary.empty()) {
				*error = "mismatch summary was empty for " + key;
				return false;
			}
			return true;
		}

		bool TestCanonicalHelpers(std::string* error) {
			const NetHash32 a = NetIdentity::HashCanonicalText("domain", {{"b", "2"}, {"a", "1"}});
			const NetHash32 b = NetIdentity::HashCanonicalText("domain", {{"a", "1"}, {"b", "2"}});
			const NetHash32 c = NetIdentity::HashCanonicalText("domain", {{"a", "1"}, {"b", "3"}});
			if (a != b) {
				*error = "canonical field sorting changed hash";
				return false;
			}
			if (a == c) {
				*error = "canonical hash did not change when a field changed";
				return false;
			}
			if (NetIdentity::HashHex(a).size() != 64 || NetIdentity::ShortHashHex(a).size() != 16) {
				*error = "hash hex lengths are wrong";
				return false;
			}

			std::string normalizeError;
			if (NetIdentity::NormalizeRelativePathForHash("Folder\\File.ini", &normalizeError) != "Folder/File.ini") {
				*error = "relative path normalization failed: " + normalizeError;
				return false;
			}
			if (!NetIdentity::NormalizeRelativePathForHash("../File.ini", &normalizeError).empty()) {
				*error = "parent-relative path was accepted";
				return false;
			}
			if (!NetIdentity::NormalizeRelativePathForHash("C:/File.ini", &normalizeError).empty()) {
				*error = "drive path was accepted";
				return false;
			}
			return true;
		}

		bool TestCompare(std::string* error) {
			const NetIdentityManifest base = MakeManifest();
			if (NetIdentity::Compare(base, base)) {
				*error = "equal manifests produced mismatch";
				return false;
			}

			NetIdentityManifest actual = base;
			actual.gameVersion = "7.0.1";
			if (!ExpectMismatchKey(base, actual, "game_version", error)) return false;

			actual = base;
			actual.buildId = "other-build";
			if (!ExpectMismatchKey(base, actual, "build_id", error)) return false;

			actual = base;
			actual.networkProtocolVersion = 2;
			if (!ExpectMismatchKey(base, actual, "network_protocol_version", error)) return false;

			actual = base;
			actual.controllerFrameVersion = 6;
			if (!ExpectMismatchKey(base, actual, "controller_frame_version", error)) return false;

			actual = base;
			actual.controllerFrameEncodedSize = 84;
			if (!ExpectMismatchKey(base, actual, "controller_frame_encoded_size", error)) return false;

			actual = base;
			actual.deterministicConfigHash = MakeHash(2);
			if (!ExpectMismatchKey(base, actual, "deterministic_config_hash", error)) return false;

			actual = base;
			actual.hasUserdataModules = true;
			if (!ExpectMismatchKey(base, actual, "userdata_modules", error)) return false;

			actual = base;
			actual.modules.pop_back();
			if (!ExpectMismatchKey(base, actual, "module_count", error)) return false;

			actual = base;
			std::swap(actual.modules[0], actual.modules[1]);
			if (!ExpectMismatchKey(base, actual, "module_order", error)) return false;

			actual = base;
			actual.modules[1].version++;
			if (!ExpectMismatchKey(base, actual, "module_metadata", error)) return false;

			actual = base;
			actual.modules[1].contentHash = MakeHash(200);
			if (!ExpectMismatchKey(base, actual, "module_content_hash", error)) return false;

			actual = base;
			actual.moduleManifestHash = MakeHash(201);
			if (!ExpectMismatchKey(base, actual, "module_manifest_hash", error)) return false;

			actual = base;
			actual.sessionRulesHash = MakeHash(202);
			if (!ExpectMismatchKey(base, actual, "session_rules_hash", error)) return false;

			actual = base;
			actual.hasUserdataModules = true;
			if (NetIdentity::Compare(base, actual, false)) {
				*error = "userdata policy override did not allow otherwise matching manifests";
				return false;
			}
			return true;
		}

		bool TestLuaStateCountOutOfIdentity(std::string* error) {
			NetIdentityDeterministicConfig four;
			four.gameVersion = "7.0.0";
			four.networkProtocolVersion = 1;
			four.controllerFrameVersion = 5;
			four.controllerFrameEncodedSize = 80;
			four.deltaTimeBits = "0x3c6147ae";
			four.aiUpdateInterval = 2;
			four.pathfinderGridNodeSize = 20;
			four.recommendedMoidCount = 240;
			four.selectedModule = "Base.rte";
			four.lockstepCodecVersion = 1;
			four.numLuaStates = 4;
			four.numLuaStatesOverride = 4;

			NetIdentityDeterministicConfig thirtyTwo = four;
			thirtyTwo.numLuaStates = 32;
			thirtyTwo.numLuaStatesOverride = 32;

			const NetHash32 configFour = NetIdentity::HashDeterministicConfig(four);
			const NetHash32 configThirtyTwo = NetIdentity::HashDeterministicConfig(thirtyTwo);

			NetIdentityManifest manifestFour = MakeManifest();
			manifestFour.deterministicConfig = four;
			manifestFour.deterministicConfigHash = configFour;
			NetIdentityManifest manifestThirtyTwo = manifestFour;
			manifestThirtyTwo.deterministicConfig = thirtyTwo;
			manifestThirtyTwo.deterministicConfigHash = configThirtyTwo;

			const NetHash32 identityFour = NetIdentity::HashSessionIdentity(manifestFour);
			const NetHash32 identityThirtyTwo = NetIdentity::HashSessionIdentity(manifestThirtyTwo);
			if (configFour != configThirtyTwo || identityFour != identityThirtyTwo) {
				*error = "the identity depends on the Lua state count: deterministic_config_hash 4 states " + NetIdentity::HashHex(configFour) +
				         " vs 32 states " + NetIdentity::HashHex(configThirtyTwo) + ", session_identity_hash 4 states " + NetIdentity::HashHex(identityFour) +
				         " vs 32 states " + NetIdentity::HashHex(identityThirtyTwo);
				return false;
			}

			if (manifestFour.deterministicConfig.numLuaStates != 4 || manifestThirtyTwo.deterministicConfig.numLuaStates != 32) {
				*error = "the Lua state count stopped being carried in the manifest";
				return false;
			}

			// The identity must still move for the config fields the sim does depend on.
			NetIdentityDeterministicConfig slowerAi = four;
			slowerAi.aiUpdateInterval = 3;
			if (NetIdentity::HashDeterministicConfig(slowerAi) == configFour) {
				*error = "deterministic_config_hash stopped reacting to ai_update_interval";
				return false;
			}
			NetIdentityManifest otherRules = manifestFour;
			otherRules.sessionRulesHash = MakeHash(203);
			if (NetIdentity::HashSessionIdentity(otherRules) == identityFour) {
				*error = "session_identity_hash stopped reacting to session_rules_hash";
				return false;
			}
			std::cout << "[net-identity-selftest] lua state count out of identity: 4 and 32 states share deterministic_config_hash "
			          << NetIdentity::HashHex(configFour) << " and session_identity_hash " << NetIdentity::HashHex(identityFour) << std::endl;
			return true;
		}
	}

	int NetIdentitySelfTest::Run() {
		std::string error;
		if (!TestCanonicalHelpers(&error) || !TestCompare(&error) || !TestLuaStateCountOutOfIdentity(&error)) {
			std::cerr << "[net-identity-selftest] FAIL: " << error << std::endl;
			return 1;
		}
		std::cout << "[net-identity-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
