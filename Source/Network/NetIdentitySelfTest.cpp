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

		NetIdentityModuleEntry MakeModule(int index, const std::string& fileName, int version, uint8_t hashSeed) {
			NetIdentityModuleEntry module;
			module.index = index;
			module.fileName = fileName;
			module.friendlyName = fileName.substr(0, fileName.find('.'));
			module.author = "selftest";
			module.version = version;
			module.official = false;
			module.root = "Mods/" + fileName;
			module.fileCount = 2;
			module.totalBytes = 42;
			module.contentHash = MakeHash(hashSeed);
			return module;
		}

		std::string NameList(const std::vector<std::string>& names) {
			std::string text;
			for (const std::string& name : names) {
				text += (text.empty() ? "" : ",") + name;
			}
			return text;
		}

		bool ExpectDiff(const std::vector<NetIdentityModuleEntry>& local, const std::vector<NetIdentityModuleEntry>& remote,
		                const std::string& missing, const std::string& extra, const std::string& differing, const std::string& what, std::string* error) {
			const NetModuleDiff diff = NetIdentity::DiffModules(
				NetIdentity::BuildModuleDigests(local, NetProtocol::c_MaxModuleDigestEntries),
				NetIdentity::BuildModuleDigests(remote, NetProtocol::c_MaxModuleDigestEntries));
			std::vector<std::string> differingNames;
			for (const NetModuleVersionDifference& difference : diff.differing) {
				differingNames.push_back(difference.fileName);
			}
			if (NameList(diff.missingOnRemote) != missing || NameList(diff.extraOnRemote) != extra || NameList(differingNames) != differing) {
				*error = what + ": expected missing=[" + missing + "] extra=[" + extra + "] differing=[" + differing + "], got missing=[" +
				         NameList(diff.missingOnRemote) + "] extra=[" + NameList(diff.extraOnRemote) + "] differing=[" + NameList(differingNames) + "]";
				return false;
			}
			return true;
		}

		bool TestDiffModules(std::string* error) {
			// T1: the host loads Base/Coalition/Ronin, the joiner Base/Ronin/MyMod with a different
			// Ronin. Name-keyed, so the extra module does not shift every module after it.
			const std::vector<NetIdentityModuleEntry> host = {
				MakeModule(0, "Base.rte", 1, 129), MakeModule(1, "Coalition.rte", 2, 145), MakeModule(2, "Ronin.rte", 5, 161)};
			std::vector<NetIdentityModuleEntry> joiner = {
				MakeModule(0, "Base.rte", 1, 129), MakeModule(1, "Ronin.rte", 3, 177), MakeModule(2, "MyMod.rte", 1, 193)};
			if (!ExpectDiff(host, joiner, "Coalition.rte", "MyMod.rte", "Ronin.rte", "T1 host to joiner", error)) return false;
			if (!ExpectDiff(joiner, host, "MyMod.rte", "Coalition.rte", "Ronin.rte", "T1 joiner to host", error)) return false;

			// The same sets with the extra module first in load order: index pairing calls this
			// module_order, the name-keyed diff still names exactly one extra and one missing module.
			std::vector<NetIdentityModuleEntry> reordered = {
				MakeModule(0, "MyMod.rte", 1, 193), MakeModule(1, "Base.rte", 1, 129), MakeModule(2, "Ronin.rte", 3, 177)};
			if (!ExpectDiff(host, reordered, "Coalition.rte", "MyMod.rte", "Ronin.rte", "T1 reordered", error)) return false;
			NetIdentityManifest indexPaired = MakeManifest();
			indexPaired.modules = host;
			NetIdentityManifest indexPairedOther = indexPaired;
			indexPairedOther.modules = reordered;
			const std::optional<NetIdentityMismatch> compare = NetIdentity::Compare(indexPaired, indexPairedOther);
			if (!compare || compare->key != "module_order") {
				*error = "index-paired Compare no longer reports module_order for a reordered list";
				return false;
			}

			// Load order alone is not a difference for the name-keyed diff.
			std::vector<NetIdentityModuleEntry> shuffled = {host[2], host[0], host[1]};
			if (!ExpectDiff(host, shuffled, "", "", "", "load order only", error)) return false;

			// A version-only difference is a difference; the content hash alone is not the whole test.
			std::vector<NetIdentityModuleEntry> newerRonin = host;
			newerRonin[2].version = 6;
			if (!ExpectDiff(host, newerRonin, "", "", "Ronin.rte", "version only", error)) return false;

			const NetModuleDiff diff = NetIdentity::DiffModules(
				NetIdentity::BuildModuleDigests(host, NetProtocol::c_MaxModuleDigestEntries),
				NetIdentity::BuildModuleDigests(joiner, NetProtocol::c_MaxModuleDigestEntries));
			if (diff.differing.size() != 1 || diff.differing[0].localVersion != 5 || diff.differing[0].remoteVersion != 3 || !diff.differing[0].contentDiffers) {
				*error = "the differing entry did not carry both versions and the content flag";
				return false;
			}
			const std::string sentence = NetIdentity::DescribeModuleDiff(diff);
			if (sentence.find("Install: Coalition.rte") == std::string::npos ||
			    sentence.find("Remove: MyMod.rte") == std::string::npos ||
			    sentence.find("Update: Ronin.rte (you 3, host 5)") == std::string::npos) {
				*error = "the joiner-facing sentence did not name the modules: \"" + sentence + "\"";
				return false;
			}
			if (!NetIdentity::DescribeModuleDiff(NetModuleDiff{}).empty()) {
				*error = "an empty diff produced a sentence";
				return false;
			}
			NetModuleDiff wide;
			for (int i = 0; i < 9; ++i) {
				wide.missingOnRemote.push_back("Mod" + std::to_string(i) + ".rte");
			}
			const std::string elided = NetIdentity::DescribeModuleDiff(wide, 6, true);
			if (elided.find("and 3 more differences") == std::string::npos) {
				*error = "a long diff did not count the differences it left out: \"" + elided + "\"";
				return false;
			}

			// The sentence rides NetJoinRejected::humanMessage, so it must always fit that field: a
			// sentence the encoder refuses would cost the joiner its refusal, not just the names.
			NetModuleDiff worst;
			for (int i = 0; i < 80; ++i) {
				const std::string name = std::to_string(100000 + i) + std::string(NetProtocol::c_MaxModuleNameBytes - 6U, 'n');
				worst.missingOnRemote.push_back(name);
				worst.extraOnRemote.push_back(name);
				worst.differing.push_back({name, 1, 2, true});
			}
			const std::string bounded = NetIdentity::DescribeModuleDiff(worst, 6, true);
			if (bounded.size() > NetProtocol::c_MaxDiagnosticTextBytes || bounded.find("more differences") == std::string::npos) {
				*error = "the worst-case sentence was " + std::to_string(bounded.size()) + " bytes, cap " +
				         std::to_string(NetProtocol::c_MaxDiagnosticTextBytes) + ": \"" + bounded + "\"";
				return false;
			}
			NetProtocolError encodeError;
			std::vector<uint8_t> encoded;
			if (!NetProtocol::Encode({1, 0, NetJoinRejected{NetRejectReason::ModuleManifestMismatch, bounded, "module_manifest_hash", "a", "b"}}, encoded, &encodeError)) {
				*error = "the worst-case sentence could not be encoded into a refusal: " + encodeError.message;
				return false;
			}

			// The digest builder is what feeds the diff: sorted, deduplicated and capped.
			bool truncated = false;
			const std::vector<NetModuleDigestEntry> capped = NetIdentity::BuildModuleDigests(host, 2, &truncated);
			if (capped.size() != 2 || capped[0].fileName != "Base.rte" || capped[1].fileName != "Coalition.rte" || !truncated) {
				*error = "the digest builder did not cap the sorted list and mark it truncated";
				return false;
			}
			std::cout << "[net-identity-selftest] PASS diff_modules by file name: " << sentence << std::endl;
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
			for (auto member : {&NetIdentityDeterministicConfig::matchConfigVersion, &NetIdentityDeterministicConfig::lobbyProtocolVersion}) {
				NetIdentityManifest incompatible = manifestFour;
				++(incompatible.deterministicConfig.*member);
				incompatible.deterministicConfigHash = NetIdentity::HashDeterministicConfig(incompatible.deterministicConfig);
				if (!ExpectMismatchKey(manifestFour, incompatible, "deterministic_config_hash", error)) return false;
			}
			NetIdentityManifest otherRules = manifestFour;
			otherRules.sessionRulesHash = MakeHash(203);
			if (NetIdentity::HashSessionIdentity(otherRules) == identityFour) {
				*error = "session_identity_hash stopped reacting to session_rules_hash";
				return false;
			}
			std::cout << "[net-identity-selftest] PASS lua state count out of identity: 4 and 32 states share deterministic_config_hash "
			          << NetIdentity::HashHex(configFour) << " and session_identity_hash " << NetIdentity::HashHex(identityFour) << std::endl;
			return true;
		}

		bool TestModuleRootOutOfIdentity(std::string* error) {
			NetIdentityManifest dataLayout = MakeManifest();
			NetIdentityManifest modsLayout = dataLayout;
			modsLayout.modules[0].root = "Mods/Base.rte";
			modsLayout.modules[1].root = "Data/Example.rte";
			dataLayout.moduleManifestHash = NetIdentity::HashModuleManifest(dataLayout.modules);
			modsLayout.moduleManifestHash = NetIdentity::HashModuleManifest(modsLayout.modules);
			const NetHash32 dataIdentity = NetIdentity::HashSessionIdentity(dataLayout);
			const NetHash32 modsIdentity = NetIdentity::HashSessionIdentity(modsLayout);
			if (dataLayout.moduleManifestHash != modsLayout.moduleManifestHash || dataIdentity != modsIdentity) {
				*error = "identities that differ only in module.root hashed differently: module_manifest_hash "
				         + NetIdentity::HashHex(dataLayout.moduleManifestHash) + " vs "
				         + NetIdentity::HashHex(modsLayout.moduleManifestHash) + ", session_identity_hash "
				         + NetIdentity::HashHex(dataIdentity) + " vs " + NetIdentity::HashHex(modsIdentity);
				return false;
			}

			NetIdentityManifest otherContent = dataLayout;
			otherContent.modules[1].contentHash = MakeHash(200);
			otherContent.moduleManifestHash = NetIdentity::HashModuleManifest(otherContent.modules);
			if (otherContent.moduleManifestHash == dataLayout.moduleManifestHash ||
			    NetIdentity::HashSessionIdentity(otherContent) == dataIdentity) {
				*error = "identities that differ in module content hashed the same";
				return false;
			}
			std::cout << "[net-identity-selftest] PASS module root out of identity: Data/ and Mods/ share module_manifest_hash "
			          << NetIdentity::HashHex(dataLayout.moduleManifestHash) << " and session_identity_hash "
			          << NetIdentity::HashHex(dataIdentity) << std::endl;
			return true;
		}
	}

	int NetIdentitySelfTest::Run() {
		std::string error;
		if (!TestCanonicalHelpers(&error) || !TestCompare(&error) || !TestDiffModules(&error) || !TestLuaStateCountOutOfIdentity(&error) || !TestModuleRootOutOfIdentity(&error)) {
			std::cerr << "[net-identity-selftest] FAIL: " << error << std::endl;
			return 1;
		}
		std::cout << "[net-identity-selftest] PASS" << std::endl;
		return 0;
	}

} // namespace RTE
