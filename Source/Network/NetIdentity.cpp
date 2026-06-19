#include "NetIdentity.h"

#include "ControllerFrame.h"
#include "DataModule.h"
#include "GameVersion.h"
#include "LuaMan.h"
#include "MovableMan.h"
#include "PresetMan.h"
#include "SettingsMan.h"
#include "System.h"
#include "TimerMan.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace RTE {

	namespace {
		using json = nlohmann::json;
		namespace fs = std::filesystem;

		struct CanonicalHasher {
			uint64_t state = 0xcbf29ce484222325ull;

			void Update(const void* data, size_t bytes) {
				const auto* p = static_cast<const uint8_t*>(data);
				for (size_t i = 0; i < bytes; ++i) {
					state ^= p[i];
					state *= 0x100000001b3ull;
				}
			}

			void UpdateString(const std::string& text) {
				Update(text.data(), text.size());
			}

			void UpdateLine(const std::string& text) {
				UpdateString(text);
				const char newline = '\n';
				Update(&newline, 1);
			}

			NetHash32 Finalize() const {
				NetHash32 out{};
				uint64_t x = state;
				for (size_t i = 0; i < out.size(); i += 8) {
					x += 0x9e3779b97f4a7c15ull;
					uint64_t z = x;
					z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
					z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
					z ^= z >> 31;
					for (size_t b = 0; b < 8 && i + b < out.size(); ++b) {
						out[i + b] = static_cast<uint8_t>(z >> (b * 8));
					}
				}
				return out;
			}
		};

		struct ModuleFileRecord {
			std::string relativePath;
			fs::path absolutePath;
			uint64_t size = 0;
		};

		std::string BoolText(bool value) {
			return value ? "1" : "0";
		}

		std::string PlatformName() {
#if defined(_WIN32)
			return "windows";
#elif defined(__APPLE__)
			return "macos";
#elif defined(__linux__)
			return "linux";
#else
			return "unknown";
#endif
		}

		std::string FloatBitsHex(float value) {
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			std::ostringstream oss;
			oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << bits;
			return oss.str();
		}

		void AppendField(CanonicalHasher& hasher, const std::string& key, const std::string& value) {
			hasher.UpdateLine("S " + key + " " + std::to_string(value.size()) + ":" + value);
		}

		void AppendInt(CanonicalHasher& hasher, const std::string& key, uint64_t value) {
			hasher.UpdateLine("I " + key + "=" + std::to_string(value));
		}

		void AppendSignedInt(CanonicalHasher& hasher, const std::string& key, int64_t value) {
			hasher.UpdateLine("I " + key + "=" + std::to_string(value));
		}

		void AppendBool(CanonicalHasher& hasher, const std::string& key, bool value) {
			hasher.UpdateLine("B " + key + "=" + BoolText(value));
		}

		void AppendHash(CanonicalHasher& hasher, const std::string& key, const NetHash32& value) {
			hasher.UpdateLine("H " + key + "=" + NetIdentity::HashHex(value));
		}

		NetHash32 HashDeterministicConfig(const NetIdentityDeterministicConfig& config) {
			CanonicalHasher hasher;
			hasher.UpdateLine("NetIdentityDeterministicConfig/v1");
			AppendField(hasher, "game_version", config.gameVersion);
			AppendInt(hasher, "network_protocol_version", config.networkProtocolVersion);
			AppendInt(hasher, "controller_frame_version", config.controllerFrameVersion);
			AppendInt(hasher, "controller_frame_encoded_size", config.controllerFrameEncodedSize);
			AppendField(hasher, "delta_time_bits", config.deltaTimeBits);
			AppendInt(hasher, "ai_update_interval", static_cast<uint64_t>(config.aiUpdateInterval));
			AppendInt(hasher, "pathfinder_grid_node_size", static_cast<uint64_t>(config.pathfinderGridNodeSize));
			AppendInt(hasher, "recommended_moid_count", static_cast<uint64_t>(config.recommendedMoidCount));
			AppendBool(hasher, "particle_settling", config.particleSettling);
			AppendBool(hasher, "mo_subtraction", config.moSubtraction);
			AppendInt(hasher, "num_lua_states", static_cast<uint64_t>(config.numLuaStates));
			AppendSignedInt(hasher, "num_lua_states_override", config.numLuaStatesOverride);
			AppendField(hasher, "selected_module", config.selectedModule);
			AppendBool(hasher, "scenario_test_module_loaded", config.scenarioTestModuleLoaded);
			return hasher.Finalize();
		}

		NetHash32 HashSessionRulesTag(const std::string& tag) {
			CanonicalHasher hasher;
			hasher.UpdateLine("NetIdentitySessionRules/v1");
			AppendField(hasher, "tag", tag);
			return hasher.Finalize();
		}

		NetHash32 HashModuleManifest(const std::vector<NetIdentityModuleEntry>& modules) {
			CanonicalHasher hasher;
			hasher.UpdateLine("NetIdentityModuleManifest/v1");
			AppendInt(hasher, "module_count", static_cast<uint64_t>(modules.size()));
			for (const NetIdentityModuleEntry& module : modules) {
				AppendInt(hasher, "module.index", static_cast<uint64_t>(module.index));
				AppendField(hasher, "module.file_name", module.fileName);
				AppendField(hasher, "module.friendly_name", module.friendlyName);
				AppendField(hasher, "module.author", module.author);
				AppendInt(hasher, "module.version", static_cast<uint64_t>(module.version));
				AppendBool(hasher, "module.official", module.official);
				AppendBool(hasher, "module.userdata", module.userdata);
				AppendField(hasher, "module.root", module.root);
				AppendInt(hasher, "module.file_count", module.fileCount);
				AppendInt(hasher, "module.total_bytes", module.totalBytes);
				AppendHash(hasher, "module.content_hash", module.contentHash);
			}
			return hasher.Finalize();
		}

		NetHash32 HashSessionIdentity(const NetIdentityManifest& manifest) {
			CanonicalHasher hasher;
			hasher.UpdateLine("NetIdentitySession/v1");
			AppendField(hasher, "game_version", manifest.gameVersion);
			AppendField(hasher, "build_id", manifest.buildId);
			AppendInt(hasher, "network_protocol_version", manifest.networkProtocolVersion);
			AppendInt(hasher, "controller_frame_version", manifest.controllerFrameVersion);
			AppendInt(hasher, "controller_frame_encoded_size", manifest.controllerFrameEncodedSize);
			AppendHash(hasher, "deterministic_config_hash", manifest.deterministicConfigHash);
			AppendHash(hasher, "module_manifest_hash", manifest.moduleManifestHash);
			AppendHash(hasher, "session_rules_hash", manifest.sessionRulesHash);
			return hasher.Finalize();
		}

		bool ReadFileIntoHasher(const fs::path& path, CanonicalHasher& hasher, std::string* error) {
			std::ifstream in(path, std::ios::binary);
			if (!in.is_open()) {
				if (error) *error = "unreadable module file: " + path.generic_string();
				return false;
			}
			std::array<char, 64 * 1024> buffer{};
			while (in) {
				in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const std::streamsize count = in.gcount();
				if (count > 0) {
					hasher.Update(buffer.data(), static_cast<size_t>(count));
				}
			}
			if (!in.eof()) {
				if (error) *error = "failed while reading module file: " + path.generic_string();
				return false;
			}
			return true;
		}

		bool CollectModuleFiles(const fs::path& root, std::vector<ModuleFileRecord>& outFiles, std::string* error) {
			std::error_code ec;
			if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
				if (error) *error = "module root is not a directory: " + root.generic_string();
				return false;
			}

			std::set<std::string> normalizedPaths;
			fs::recursive_directory_iterator it(root, fs::directory_options::none, ec);
			const fs::recursive_directory_iterator end;
			if (ec) {
				if (error) *error = "failed to open module root: " + root.generic_string() + ": " + ec.message();
				return false;
			}

			for (; it != end; it.increment(ec)) {
				if (ec) {
					if (error) *error = "failed while walking module root: " + root.generic_string() + ": " + ec.message();
					return false;
				}

				const fs::directory_entry& entry = *it;
				const fs::file_status symlinkStatus = entry.symlink_status(ec);
				if (ec) {
					if (error) *error = "failed to stat module path: " + entry.path().generic_string() + ": " + ec.message();
					return false;
				}
				if (fs::is_symlink(symlinkStatus)) {
					if (error) *error = "symlink in module content is not allowed: " + entry.path().generic_string();
					return false;
				}
				const fs::file_status status = entry.status(ec);
				if (ec) {
					if (error) *error = "failed to stat module path: " + entry.path().generic_string() + ": " + ec.message();
					return false;
				}
				if (fs::is_directory(status)) {
					continue;
				}
				if (!fs::is_regular_file(status)) {
					if (error) *error = "non-regular file in module content is not allowed: " + entry.path().generic_string();
					return false;
				}

				std::string relativeError;
				const std::string relativePath = NetIdentity::NormalizeRelativePathForHash(entry.path().lexically_relative(root).generic_string(), &relativeError);
				if (relativePath.empty()) {
					if (error) *error = "invalid module relative path: " + entry.path().generic_string() + ": " + relativeError;
					return false;
				}
				if (!normalizedPaths.insert(relativePath).second) {
					if (error) *error = "module path normalization collision: " + relativePath;
					return false;
				}
				const uintmax_t size = entry.file_size(ec);
				if (ec) {
					if (error) *error = "failed to get file size: " + entry.path().generic_string() + ": " + ec.message();
					return false;
				}
				outFiles.push_back({relativePath, entry.path(), static_cast<uint64_t>(size)});
			}

			std::sort(outFiles.begin(), outFiles.end(), [](const ModuleFileRecord& a, const ModuleFileRecord& b) {
				return a.relativePath < b.relativePath;
			});
			return true;
		}

		bool HashModuleContent(const NetIdentityModuleEntry& module, const fs::path& root, NetHash32& outHash, std::string* error) {
			std::vector<ModuleFileRecord> files;
			if (!CollectModuleFiles(root, files, error)) {
				return false;
			}

			CanonicalHasher hasher;
			hasher.UpdateLine("NetIdentityModuleContent/v1");
			AppendInt(hasher, "module.index", static_cast<uint64_t>(module.index));
			AppendField(hasher, "module.file_name", module.fileName);
			AppendBool(hasher, "module.official", module.official);
			AppendBool(hasher, "module.userdata", module.userdata);
			AppendInt(hasher, "file_count", static_cast<uint64_t>(files.size()));

			for (const ModuleFileRecord& file : files) {
				AppendField(hasher, "file.path", file.relativePath);
				AppendInt(hasher, "file.size", file.size);
				if (!ReadFileIntoHasher(file.absolutePath, hasher, error)) {
					return false;
				}
			}

			outHash = hasher.Finalize();
			return true;
		}

		json HashJson(const NetHash32& hash) {
			return NetIdentity::HashHex(hash);
		}

		json DeterministicConfigJson(const NetIdentityDeterministicConfig& config) {
			return json{
				{"game_version", config.gameVersion},
				{"network_protocol_version", config.networkProtocolVersion},
				{"controller_frame_version", config.controllerFrameVersion},
				{"controller_frame_encoded_size", config.controllerFrameEncodedSize},
				{"delta_time_bits", config.deltaTimeBits},
				{"ai_update_interval", config.aiUpdateInterval},
				{"pathfinder_grid_node_size", config.pathfinderGridNodeSize},
				{"recommended_moid_count", config.recommendedMoidCount},
				{"particle_settling", config.particleSettling},
				{"mo_subtraction", config.moSubtraction},
				{"num_lua_states", config.numLuaStates},
				{"num_lua_states_override", config.numLuaStatesOverride},
				{"selected_module", config.selectedModule},
				{"scenario_test_module_loaded", config.scenarioTestModuleLoaded},
			};
		}

		json ModuleJson(const NetIdentityModuleEntry& module) {
			return json{
				{"index", module.index},
				{"file_name", module.fileName},
				{"friendly_name", module.friendlyName},
				{"author", module.author},
				{"version", module.version},
				{"official", module.official},
				{"userdata", module.userdata},
				{"root", module.root},
				{"file_count", module.fileCount},
				{"total_bytes", module.totalBytes},
				{"content_hash", HashJson(module.contentHash)},
			};
		}

		json ManifestJson(const NetIdentityManifest& manifest) {
			json modules = json::array();
			for (const NetIdentityModuleEntry& module : manifest.modules) {
				modules.push_back(ModuleJson(module));
			}
			return json{
				{"schema", manifest.schema},
				{"game_version", manifest.gameVersion},
				{"network_protocol_version", manifest.networkProtocolVersion},
				{"controller_frame_version", manifest.controllerFrameVersion},
				{"controller_frame_encoded_size", manifest.controllerFrameEncodedSize},
				{"build_id", manifest.buildId},
				{"platform", manifest.platform},
				{"deterministic_config", DeterministicConfigJson(manifest.deterministicConfig)},
				{"modules", modules},
				{"has_userdata_modules", manifest.hasUserdataModules},
				{"deterministic_config_hash", HashJson(manifest.deterministicConfigHash)},
				{"module_manifest_hash", HashJson(manifest.moduleManifestHash)},
				{"session_rules_hash", HashJson(manifest.sessionRulesHash)},
				{"session_identity_hash", HashJson(manifest.sessionIdentityHash)},
				{"hash_duration_ms", manifest.hashDurationMs},
				{"warnings", manifest.warnings},
			};
		}

		std::string ShortText(const std::string& text) {
			constexpr size_t c_Max = 24;
			if (text.size() <= c_Max) {
				return text;
			}
			return text.substr(0, c_Max);
		}

		NetIdentityMismatch MakeMismatch(const std::string& key, NetRejectReason reason, const std::string& expected, const std::string& actual, std::string summary) {
			return NetIdentityMismatch{key, reason, ShortText(expected), ShortText(actual), "", "", std::move(summary)};
		}

		NetIdentityMismatch MakeHashMismatch(const std::string& key, NetRejectReason reason, const NetHash32& expected, const NetHash32& actual, std::string summary) {
			return NetIdentityMismatch{key, reason, NetIdentity::ShortHashHex(expected), NetIdentity::ShortHashHex(actual), "", "", std::move(summary)};
		}

		std::string ModuleLabel(const NetIdentityModuleEntry& module) {
			return std::to_string(module.index) + ":" + module.fileName;
		}
	}

	bool NetIdentity::BuildCurrentManifest(NetIdentityManifest& outManifest, std::string* error, NetIdentityBuildOptions options) {
		const auto started = std::chrono::steady_clock::now();

		NetIdentityManifest manifest;
		manifest.schema = 1;
		manifest.gameVersion = c_VersionString;
		manifest.networkProtocolVersion = NetProtocol::c_Version;
		manifest.controllerFrameVersion = ControllerFrame::c_Version;
		manifest.controllerFrameEncodedSize = ControllerFrame::c_EncodedSize;
		manifest.buildId = options.buildId;
		manifest.platform = PlatformName();

		manifest.deterministicConfig.gameVersion = manifest.gameVersion;
		manifest.deterministicConfig.networkProtocolVersion = manifest.networkProtocolVersion;
		manifest.deterministicConfig.controllerFrameVersion = manifest.controllerFrameVersion;
		manifest.deterministicConfig.controllerFrameEncodedSize = manifest.controllerFrameEncodedSize;
		manifest.deterministicConfig.deltaTimeBits = FloatBitsHex(g_TimerMan.GetDeltaTimeSecs());
		manifest.deterministicConfig.aiUpdateInterval = g_SettingsMan.GetAIUpdateInterval();
		manifest.deterministicConfig.pathfinderGridNodeSize = g_SettingsMan.GetPathFinderGridNodeSize();
		manifest.deterministicConfig.recommendedMoidCount = g_SettingsMan.RecommendedMOIDCount();
		manifest.deterministicConfig.particleSettling = g_MovableMan.IsParticleSettlingEnabled();
		manifest.deterministicConfig.moSubtraction = g_MovableMan.IsMOSubtractionEnabled();
		manifest.deterministicConfig.numLuaStates = static_cast<int>(g_LuaMan.GetThreadedScriptStates().size());
		manifest.deterministicConfig.numLuaStatesOverride = g_SettingsMan.GetNumberOfLuaStatesOverride();
		manifest.deterministicConfig.selectedModule = g_PresetMan.GetSingleModuleToLoad();
		manifest.deterministicConfig.scenarioTestModuleLoaded = g_PresetMan.GetModuleID("Tests.rte") >= 0;

		const std::string workingDirectory = System::GetWorkingDirectory();
		const int moduleCount = g_PresetMan.GetTotalModuleCount();
		manifest.modules.reserve(static_cast<size_t>(moduleCount));
		for (int i = 0; i < moduleCount; ++i) {
			const DataModule* dataModule = g_PresetMan.GetDataModule(i);
			if (!dataModule) {
				if (error) *error = "null data module at index " + std::to_string(i);
				return false;
			}

			NetIdentityModuleEntry module;
			module.index = i;
			module.fileName = dataModule->GetFileName();
			module.friendlyName = dataModule->GetFriendlyName();
			module.author = dataModule->GetAuthor();
			module.version = dataModule->GetVersionNumber();
			module.official = g_PresetMan.IsModuleOfficial(module.fileName);
			module.userdata = dataModule->IsUserdata();
			module.root = fs::path(g_PresetMan.GetFullModulePath(module.fileName)).generic_string();

			if (module.userdata) {
				manifest.hasUserdataModules = true;
				if (!options.includeUserdataModules) {
					manifest.warnings.push_back("userdata module " + module.fileName + " omitted from network module manifest");
					continue;
				}
			}

			const fs::path rootAbsolute = fs::path(workingDirectory) / fs::path(module.root);
			std::vector<ModuleFileRecord> files;
			if (!CollectModuleFiles(rootAbsolute, files, error)) {
				return false;
			}
			module.fileCount = static_cast<uint64_t>(files.size());
			for (const ModuleFileRecord& file : files) {
				module.totalBytes += file.size;
			}
			if (!HashModuleContent(module, rootAbsolute, module.contentHash, error)) {
				return false;
			}

			const std::string zipCandidate = module.root + ".zip";
			if (fs::exists(fs::path(workingDirectory) / fs::path(zipCandidate))) {
				manifest.warnings.push_back("module " + module.fileName + " was hashed from extracted directory; zip canonicalization remains a P2D follow-up");
			}
			manifest.modules.push_back(std::move(module));
		}

		manifest.deterministicConfigHash = HashDeterministicConfig(manifest.deterministicConfig);
		manifest.moduleManifestHash = HashModuleManifest(manifest.modules);
		manifest.sessionRulesHash = HashSessionRulesTag(options.sessionRulesTag);
		manifest.sessionIdentityHash = HashSessionIdentity(manifest);
		manifest.hashDurationMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());

		outManifest = std::move(manifest);
		return true;
	}

	bool NetIdentity::WriteManifestJson(const NetIdentityManifest& manifest, const std::string& path, std::string* error) {
		std::ofstream out(path, std::ios::binary);
		if (!out.is_open()) {
			if (error) *error = "failed to open identity dump for writing: " + path;
			return false;
		}
		out << ManifestJson(manifest).dump(2) << '\n';
		if (!out.good()) {
			if (error) *error = "failed to write identity dump: " + path;
			return false;
		}
		return true;
	}

	bool NetIdentity::DumpCurrentManifestJson(const std::string& path, std::string* error, NetIdentityManifest* outManifest) {
		NetIdentityManifest manifest;
		if (!BuildCurrentManifest(manifest, error)) {
			return false;
		}
		if (!WriteManifestJson(manifest, path, error)) {
			return false;
		}
		if (outManifest) {
			*outManifest = manifest;
		}
		return true;
	}

	std::optional<NetIdentityMismatch> NetIdentity::Compare(const NetIdentityManifest& expected, const NetIdentityManifest& actual, bool rejectUserdataModules) {
		if (expected.gameVersion != actual.gameVersion) {
			return MakeMismatch("game_version", NetRejectReason::GameVersionMismatch, expected.gameVersion, actual.gameVersion, "game version does not match");
		}
		if (expected.buildId != actual.buildId) {
			return MakeMismatch("build_id", NetRejectReason::BuildMismatch, expected.buildId, actual.buildId, "build id does not match");
		}
		if (expected.networkProtocolVersion != actual.networkProtocolVersion) {
			return MakeMismatch("network_protocol_version", NetRejectReason::ProtocolMismatch, std::to_string(expected.networkProtocolVersion), std::to_string(actual.networkProtocolVersion), "network protocol version does not match");
		}
		if (expected.controllerFrameVersion != actual.controllerFrameVersion) {
			return MakeMismatch("controller_frame_version", NetRejectReason::ControllerFrameVersionMismatch, std::to_string(expected.controllerFrameVersion), std::to_string(actual.controllerFrameVersion), "ControllerFrame version does not match");
		}
		if (expected.controllerFrameEncodedSize != actual.controllerFrameEncodedSize) {
			return MakeMismatch("controller_frame_encoded_size", NetRejectReason::ControllerFrameSizeMismatch, std::to_string(expected.controllerFrameEncodedSize), std::to_string(actual.controllerFrameEncodedSize), "ControllerFrame encoded size does not match");
		}
		if (expected.deterministicConfigHash != actual.deterministicConfigHash) {
			return MakeHashMismatch("deterministic_config_hash", NetRejectReason::DeterministicConfigMismatch, expected.deterministicConfigHash, actual.deterministicConfigHash, "deterministic config hash does not match");
		}
		if (rejectUserdataModules && actual.hasUserdataModules) {
			return MakeMismatch("userdata_modules", NetRejectReason::UserdataModulesNotAllowed, "false", "true", "userdata modules are not allowed in network sessions");
		}
		if (expected.modules.size() != actual.modules.size()) {
			return MakeMismatch("module_count", NetRejectReason::ModuleManifestMismatch, std::to_string(expected.modules.size()), std::to_string(actual.modules.size()), "loaded module count does not match");
		}
		for (size_t i = 0; i < expected.modules.size(); ++i) {
			const NetIdentityModuleEntry& expectedModule = expected.modules[i];
			const NetIdentityModuleEntry& actualModule = actual.modules[i];
			if (expectedModule.fileName != actualModule.fileName) {
				NetIdentityMismatch mismatch = MakeMismatch("module_order", NetRejectReason::ModuleManifestMismatch, ModuleLabel(expectedModule), ModuleLabel(actualModule), "loaded module order does not match");
				mismatch.moduleName = actualModule.fileName;
				return mismatch;
			}
			if (expectedModule.friendlyName != actualModule.friendlyName ||
			    expectedModule.author != actualModule.author ||
			    expectedModule.version != actualModule.version ||
			    expectedModule.official != actualModule.official ||
			    expectedModule.userdata != actualModule.userdata ||
			    expectedModule.root != actualModule.root) {
				NetIdentityMismatch mismatch = MakeMismatch("module_metadata", NetRejectReason::ModuleManifestMismatch, ModuleLabel(expectedModule), ModuleLabel(actualModule), "module metadata does not match");
				mismatch.moduleName = actualModule.fileName;
				return mismatch;
			}
			if (expectedModule.contentHash != actualModule.contentHash) {
				NetIdentityMismatch mismatch = MakeHashMismatch("module_content_hash", NetRejectReason::ModuleManifestMismatch, expectedModule.contentHash, actualModule.contentHash, "module content hash does not match");
				mismatch.moduleName = actualModule.fileName;
				return mismatch;
			}
		}
		if (expected.moduleManifestHash != actual.moduleManifestHash) {
			return MakeHashMismatch("module_manifest_hash", NetRejectReason::ModuleManifestMismatch, expected.moduleManifestHash, actual.moduleManifestHash, "module manifest hash does not match");
		}
		if (expected.sessionRulesHash != actual.sessionRulesHash) {
			return MakeHashMismatch("session_rules_hash", NetRejectReason::SessionRulesMismatch, expected.sessionRulesHash, actual.sessionRulesHash, "session rules hash does not match");
		}
		return std::nullopt;
	}

	NetHash32 NetIdentity::HashCanonicalText(const std::string& domain, const std::vector<std::pair<std::string, std::string>>& fields) {
		CanonicalHasher hasher;
		hasher.UpdateLine("NetIdentityCanonicalText/v1");
		AppendField(hasher, "domain", domain);
		std::vector<std::pair<std::string, std::string>> sortedFields = fields;
		std::sort(sortedFields.begin(), sortedFields.end());
		for (const auto& [key, value] : sortedFields) {
			AppendField(hasher, key, value);
		}
		return hasher.Finalize();
	}

	std::string NetIdentity::HashHex(const NetHash32& hash) {
		static const char hex[] = "0123456789abcdef";
		std::string out;
		out.resize(hash.size() * 2);
		for (size_t i = 0; i < hash.size(); ++i) {
			out[i * 2 + 0] = hex[(hash[i] >> 4) & 0x0F];
			out[i * 2 + 1] = hex[hash[i] & 0x0F];
		}
		return out;
	}

	std::string NetIdentity::ShortHashHex(const NetHash32& hash) {
		return HashHex(hash).substr(0, 16);
	}

	std::string NetIdentity::NormalizeRelativePathForHash(const std::string& path, std::string* error) {
		if (path.empty()) {
			if (error) *error = "path is empty";
			return "";
		}
		std::string normalized = fs::path(path).generic_string();
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty() || normalized[0] == '/') {
			if (error) *error = "path is absolute";
			return "";
		}
		if (normalized.find(':') != std::string::npos) {
			if (error) *error = "path contains drive or stream separator";
			return "";
		}
		std::stringstream ss(normalized);
		std::string segment;
		while (std::getline(ss, segment, '/')) {
			if (segment.empty() || segment == "." || segment == "..") {
				if (error) *error = "path contains invalid segment";
				return "";
			}
		}
		return normalized;
	}

} // namespace RTE
