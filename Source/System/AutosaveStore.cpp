#include "AutosaveStore.h"

#include "SaveGameArchive.h"
#include "System.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <system_error>

namespace RTE {
	namespace {
		const char* c_RequiredEntries[] = {"Index.ini", "Save Mat.png", "Save FG.png", "Save BG.png"};

		std::string Trim(std::string_view text) {
			const size_t first = text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) return {};
			return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
		}

		/// One "Key = Value" line per field; a value never carries a newline, so the text needs no escaping.
		void Line(std::ostringstream& out, const char* key, const std::string& value) {
			out << key << " = " << value << "\n";
		}

		bool ParseNumber(const std::string& text, uint64_t& out) {
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
			return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
		}

		/// The newest checkpoint this process published and validated, kept so a heal names the rewind point
		/// without reading the disk on the game thread. The archive thread writes it, the game thread reads it.
		std::mutex s_ValidatedMutex;
		AutosaveDescriptor s_Validated;

		/// The tick the saved world itself stands on, read out of the checkpoint's own property.
		bool WorldTick(const std::string& saveText, uint64_t& out) {
			static constexpr std::string_view Key = "SimUpdateCount = ";
			for (size_t start = 0; (start = saveText.find(Key, start)) != std::string::npos; start += Key.size()) {
				const bool lineStart = start == 0 || saveText[start - 1] == '\n' || saveText[start - 1] == '\t' || saveText[start - 1] == ' ';
				if (!lineStart) continue;
				const size_t valueStart = start + Key.size();
				const size_t end = saveText.find_first_of("\r\n", valueStart);
				return ParseNumber(Trim(std::string_view(saveText).substr(valueStart, end == std::string::npos ? end : end - valueStart)), out);
			}
			return false;
		}
	}

	std::filesystem::path AutosaveStore::Directory() {
		return std::filesystem::path(System::GetWorkingDirectory()) / "Autosaves";
	}

	std::string AutosaveStore::ArchiveName(const std::string& matchId, uint64_t tick) {
		return matchId + "-" + std::to_string(tick) + c_ArchiveExtension;
	}

	std::filesystem::path AutosaveStore::ArchivePath(const std::filesystem::path& directory, const std::string& matchId, uint64_t tick) {
		return directory / ArchiveName(matchId, tick);
	}

	std::filesystem::path AutosaveStore::ArchivePath(const std::string& matchId, uint64_t tick) {
		return ArchivePath(Directory(), matchId, tick);
	}

	bool AutosaveStore::ValidMatchId(const std::string& matchId) {
		return !matchId.empty() && matchId.size() <= c_MaxMatchIdBytes &&
		       matchId.find_first_not_of("0123456789abcdef-") == std::string::npos;
	}

	bool AutosaveStore::ParseArchiveName(const std::string& fileName, const std::string& matchId, uint64_t& outTick) {
		const std::string prefix = matchId + "-";
		const std::string extension = c_ArchiveExtension;
		if (!ValidMatchId(matchId) || !fileName.starts_with(prefix) || !fileName.ends_with(extension) ||
		    fileName.size() <= prefix.size() + extension.size()) {
			return false;
		}
		return ParseNumber(fileName.substr(prefix.size(), fileName.size() - prefix.size() - extension.size()), outTick);
	}

	std::string AutosaveStore::WriteDescriptor(const AutosaveDescriptor& descriptor) {
		std::ostringstream out;
		Line(out, "RestoreSchema", std::to_string(c_DescriptorSchema));
		Line(out, "MatchId", descriptor.matchId);
		Line(out, "SessionId", std::to_string(descriptor.sessionId));
		Line(out, "RoundId", std::to_string(descriptor.roundId));
		Line(out, "SavedTick", std::to_string(descriptor.savedTick));
		Line(out, "SimTimeTicks", std::to_string(descriptor.simTimeTicks));
		Line(out, "IntervalSeconds", std::to_string(descriptor.intervalSeconds));
		Line(out, "GameVersion", descriptor.gameVersion);
		Line(out, "BuildId", descriptor.buildId);
		Line(out, "DeterministicConfigHash", descriptor.deterministicConfigHash);
		Line(out, "ModuleManifestHash", descriptor.moduleManifestHash);
		Line(out, "SessionIdentityHash", descriptor.sessionIdentityHash);
		Line(out, "WorldStructureHash", descriptor.worldStructureHash);
		Line(out, "ActivityPreset", descriptor.activityPreset);
		Line(out, "ScenePreset", descriptor.scenePreset);
		return out.str();
	}

	bool AutosaveStore::ParseDescriptor(const std::string& text, AutosaveDescriptor& out, std::string* error) {
		AutosaveDescriptor parsed;
		bool hasSchema = false, hasTick = false;
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line)) {
			const size_t separator = line.find('=');
			if (separator == std::string::npos) continue;
			const std::string key = Trim(std::string_view(line).substr(0, separator));
			const std::string value = Trim(std::string_view(line).substr(separator + 1));
			uint64_t number = 0;
			if (key == "RestoreSchema") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable RestoreSchema"; return false; }
				parsed.schema = static_cast<int>(number);
				hasSchema = true;
			} else if (key == "MatchId") {
				parsed.matchId = value;
			} else if (key == "SessionId") {
				if (!ParseNumber(value, parsed.sessionId)) { if (error) *error = "unreadable SessionId"; return false; }
			} else if (key == "RoundId") {
				if (!ParseNumber(value, parsed.roundId)) { if (error) *error = "unreadable RoundId"; return false; }
			} else if (key == "SavedTick") {
				if (!ParseNumber(value, parsed.savedTick)) { if (error) *error = "unreadable SavedTick"; return false; }
				hasTick = true;
			} else if (key == "SimTimeTicks") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable SimTimeTicks"; return false; }
				parsed.simTimeTicks = static_cast<long long>(number);
			} else if (key == "IntervalSeconds") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable IntervalSeconds"; return false; }
				parsed.intervalSeconds = static_cast<uint32_t>(number);
			} else if (key == "GameVersion") {
				parsed.gameVersion = value;
			} else if (key == "BuildId") {
				parsed.buildId = value;
			} else if (key == "DeterministicConfigHash") {
				parsed.deterministicConfigHash = value;
			} else if (key == "ModuleManifestHash") {
				parsed.moduleManifestHash = value;
			} else if (key == "SessionIdentityHash") {
				parsed.sessionIdentityHash = value;
			} else if (key == "WorldStructureHash") {
				parsed.worldStructureHash = value;
			} else if (key == "ActivityPreset") {
				parsed.activityPreset = value;
			} else if (key == "ScenePreset") {
				parsed.scenePreset = value;
			}
		}
		if (!hasSchema || parsed.schema != c_DescriptorSchema) {
			if (error) *error = "unsupported restore schema " + std::to_string(parsed.schema);
			return false;
		}
		if (!hasTick || parsed.savedTick == 0 || !ValidMatchId(parsed.matchId)) {
			if (error) *error = "descriptor has no match id or committed tick";
			return false;
		}
		out = std::move(parsed);
		return true;
	}

	bool AutosaveStore::Validate(const std::filesystem::path& path, AutosaveDescriptor& out, std::string* error) {
		try {
			std::error_code status;
			if (!std::filesystem::is_regular_file(path, status)) {
				if (error) *error = "not a regular file";
				return false;
			}
			SaveGameArchive archive(path.string());
			std::string descriptorText;
			archive.ReadEntry(c_DescriptorEntry, descriptorText);
			AutosaveDescriptor descriptor;
			if (!ParseDescriptor(descriptorText, descriptor, error)) return false;
			std::string entry;
			// Reading an entry to its end is what checks its CRC, so a torn write is caught here.
			for (const char* name: c_RequiredEntries) archive.ReadEntry(name, entry);
			uint64_t worldTick = 0;
			std::string saveText;
			archive.ReadEntry("Save.ini", saveText);
			if (saveText.empty()) {
				if (error) *error = "the checkpoint carries no world";
				return false;
			}
			if (!WorldTick(saveText, worldTick)) {
				if (error) *error = "the world carries no SimUpdateCount";
				return false;
			}
			uint64_t nameTick = 0;
			if (!ParseArchiveName(path.filename().string(), descriptor.matchId, nameTick)) {
				if (error) *error = "file name does not name this match and tick";
				return false;
			}
			if (nameTick != descriptor.savedTick || worldTick != descriptor.savedTick) {
				if (error) {
					*error = "tick disagrees: name " + std::to_string(nameTick) + ", descriptor " +
					         std::to_string(descriptor.savedTick) + ", world " + std::to_string(worldTick);
				}
				return false;
			}
			descriptor.path = path;
			// Restorable says the world reads; resumable says a restarted host can also reopen the lobby
			// it belongs to, which needs the checkpoint's manifest and the match's admission file.
			AutosaveManifest manifest;
			AutosaveAdmission admission;
			descriptor.resumable = ReadManifest(path.parent_path(), descriptor.matchId, descriptor.savedTick, manifest) &&
			                       manifest.savedTick == descriptor.savedTick && !manifest.configPayload.empty() &&
			                       ReadAdmission(path.parent_path(), descriptor.matchId, admission);
			out = std::move(descriptor);
			return true;
		} catch (const std::exception& exception) {
			if (error) *error = exception.what();
			return false;
		}
	}

	std::filesystem::path AutosaveStore::ManifestPath(const std::filesystem::path& directory, const std::string& matchId, uint64_t tick) {
		return directory / (matchId + "-" + std::to_string(tick) + c_ManifestExtension);
	}

	std::filesystem::path AutosaveStore::AdmissionPath(const std::filesystem::path& directory, const std::string& matchId) {
		return directory / (matchId + c_AdmissionExtension);
	}

	std::string AutosaveStore::WriteManifest(const AutosaveManifest& manifest) {
		std::ostringstream out;
		Line(out, "ManifestSchema", std::to_string(c_ManifestSchema));
		Line(out, "MatchId", manifest.matchId);
		Line(out, "SessionId", std::to_string(manifest.sessionId));
		Line(out, "RoundId", std::to_string(manifest.roundId));
		Line(out, "SavedTick", std::to_string(manifest.savedTick));
		Line(out, "SimTimeTicks", std::to_string(manifest.simTimeTicks));
		Line(out, "IntervalSeconds", std::to_string(manifest.intervalSeconds));
		Line(out, "ConfigHash", manifest.configHash);
		Line(out, "ConfigPayload", manifest.configPayload);
		Line(out, "ActivityPreset", manifest.activityPreset);
		Line(out, "ScenePreset", manifest.scenePreset);
		for (const std::string& name: manifest.peerNames) Line(out, "Peer", name);
		return out.str();
	}

	bool AutosaveStore::ParseManifest(const std::string& text, AutosaveManifest& out, std::string* error) {
		AutosaveManifest parsed;
		bool hasSchema = false;
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line)) {
			const size_t separator = line.find('=');
			if (separator == std::string::npos) continue;
			const std::string key = Trim(std::string_view(line).substr(0, separator));
			const std::string value = Trim(std::string_view(line).substr(separator + 1));
			uint64_t number = 0;
			if (key == "ManifestSchema") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable ManifestSchema"; return false; }
				parsed.schema = static_cast<int>(number);
				hasSchema = true;
			} else if (key == "MatchId") {
				parsed.matchId = value;
			} else if (key == "SessionId") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable SessionId"; return false; }
				parsed.sessionId = number;
			} else if (key == "RoundId") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable RoundId"; return false; }
				parsed.roundId = number;
			} else if (key == "SavedTick") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable SavedTick"; return false; }
				parsed.savedTick = number;
			} else if (key == "SimTimeTicks") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable SimTimeTicks"; return false; }
				parsed.simTimeTicks = static_cast<long long>(number);
			} else if (key == "IntervalSeconds") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable IntervalSeconds"; return false; }
				parsed.intervalSeconds = static_cast<uint32_t>(number);
			} else if (key == "ConfigHash") {
				parsed.configHash = value;
			} else if (key == "ConfigPayload") {
				parsed.configPayload = value;
			} else if (key == "ActivityPreset") {
				parsed.activityPreset = value;
			} else if (key == "ScenePreset") {
				parsed.scenePreset = value;
			} else if (key == "Peer") {
				parsed.peerNames.push_back(value);
			}
		}
		if (!hasSchema || parsed.schema != c_ManifestSchema) {
			if (error) *error = "unsupported manifest schema " + std::to_string(parsed.schema);
			return false;
		}
		if (parsed.savedTick == 0 || !ValidMatchId(parsed.matchId) || parsed.configPayload.empty()) {
			if (error) *error = "manifest has no match id, committed tick or configuration";
			return false;
		}
		if (parsed.configPayload.size() % 2 != 0 || parsed.configPayload.find_first_not_of("0123456789abcdef") != std::string::npos) {
			if (error) *error = "manifest configuration is not hex";
			return false;
		}
		out = std::move(parsed);
		return true;
	}

	namespace {
		/// Writes the text to a temporary neighbour and renames it over the target, so a reader sees the
		/// previous generation or this one, never half of either.
		bool PublishFile(const std::filesystem::path& path, const std::string& text, std::string* error) {
			std::error_code status;
			std::filesystem::create_directories(path.parent_path(), status);
			const std::filesystem::path temporary = path.string() + ".tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				out.write(text.data(), static_cast<std::streamsize>(text.size()));
				out.close();
				if (!out.good()) {
					if (error) *error = "could not write " + temporary.string();
					std::filesystem::remove(temporary, status);
					return false;
				}
			}
			std::filesystem::rename(temporary, path, status);
			if (status) {
				if (error) *error = "could not publish " + path.string() + ": " + status.message();
				std::error_code ignored;
				std::filesystem::remove(temporary, ignored);
				return false;
			}
			return true;
		}

		bool ReadFileText(const std::filesystem::path& path, std::string& out, std::string* error) {
			std::error_code status;
			if (!std::filesystem::is_regular_file(path, status)) {
				if (error) *error = "not a regular file: " + path.string();
				return false;
			}
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				if (error) *error = "could not read " + path.string();
				return false;
			}
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return true;
		}

		std::string ToHex(const std::vector<uint8_t>& bytes) {
			static constexpr char Digits[] = "0123456789abcdef";
			std::string hex;
			hex.reserve(bytes.size() * 2);
			for (uint8_t byte: bytes) {
				hex.push_back(Digits[byte >> 4]);
				hex.push_back(Digits[byte & 0x0F]);
			}
			return hex;
		}

		bool FromHex(const std::string& hex, std::vector<uint8_t>& out) {
			if (hex.size() % 2 != 0) return false;
			out.clear();
			out.reserve(hex.size() / 2);
			for (size_t index = 0; index < hex.size(); index += 2) {
				uint8_t value = 0;
				for (size_t half = 0; half < 2; ++half) {
					const char digit = hex[index + half];
					const int nibble = digit >= '0' && digit <= '9' ? digit - '0' : (digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : -1);
					if (nibble < 0) return false;
					value = static_cast<uint8_t>((value << 4) | static_cast<uint8_t>(nibble));
				}
				out.push_back(value);
			}
			return true;
		}
	}

	bool AutosaveStore::PublishManifest(const std::filesystem::path& directory, const AutosaveManifest& manifest, std::string* error) {
		if (!ValidMatchId(manifest.matchId) || manifest.savedTick == 0 || manifest.configPayload.empty()) {
			if (error) *error = "manifest has no match id, committed tick or configuration";
			return false;
		}
		return PublishFile(ManifestPath(directory, manifest.matchId, manifest.savedTick), WriteManifest(manifest), error);
	}

	bool AutosaveStore::ReadManifest(const std::filesystem::path& path, AutosaveManifest& out, std::string* error) {
		std::string text;
		if (!ReadFileText(path, text, error) || !ParseManifest(text, out, error)) return false;
		out.path = path;
		return true;
	}

	bool AutosaveStore::ReadManifest(const std::filesystem::path& directory, const std::string& matchId, uint64_t tick, AutosaveManifest& out, std::string* error) {
		if (!ValidMatchId(matchId)) {
			if (error) *error = "no match id";
			return false;
		}
		return ReadManifest(ManifestPath(directory, matchId, tick), out, error);
	}

	bool AutosaveStore::PublishAdmission(const std::filesystem::path& directory, const AutosaveAdmission& admission, std::string* error) {
		if (!ValidMatchId(admission.matchId) || admission.sealed.empty() || admission.sealed.size() > c_MaxAdmissionBytes) {
			if (error) *error = "admission has no match id or no sealed export";
			return false;
		}
		// A generation never rewinds, so a file written by an older export cannot replace a newer one.
		AutosaveAdmission held;
		if (ReadAdmission(directory, admission.matchId, held) && held.generation > admission.generation) {
			if (error) *error = "a newer admission generation is already published";
			return false;
		}
		std::ostringstream out;
		Line(out, "AdmissionSchema", std::to_string(c_AdmissionSchema));
		Line(out, "MatchId", admission.matchId);
		Line(out, "Generation", std::to_string(admission.generation));
		Line(out, "Sealed", ToHex(admission.sealed));
		return PublishFile(AdmissionPath(directory, admission.matchId), out.str(), error);
	}

	bool AutosaveStore::ReadAdmission(const std::filesystem::path& directory, const std::string& matchId, AutosaveAdmission& out, std::string* error) {
		if (!ValidMatchId(matchId)) {
			if (error) *error = "no match id";
			return false;
		}
		const std::filesystem::path path = AdmissionPath(directory, matchId);
		std::string text;
		if (!ReadFileText(path, text, error)) return false;
		AutosaveAdmission parsed;
		bool hasSchema = false;
		std::string sealedHex;
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line)) {
			const size_t separator = line.find('=');
			if (separator == std::string::npos) continue;
			const std::string key = Trim(std::string_view(line).substr(0, separator));
			const std::string value = Trim(std::string_view(line).substr(separator + 1));
			uint64_t number = 0;
			if (key == "AdmissionSchema") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable AdmissionSchema"; return false; }
				parsed.schema = static_cast<int>(number);
				hasSchema = true;
			} else if (key == "MatchId") {
				parsed.matchId = value;
			} else if (key == "Generation") {
				if (!ParseNumber(value, number)) { if (error) *error = "unreadable Generation"; return false; }
				parsed.generation = number;
			} else if (key == "Sealed") {
				sealedHex = value;
			}
		}
		if (!hasSchema || parsed.schema != c_AdmissionSchema) {
			if (error) *error = "unsupported admission schema " + std::to_string(parsed.schema);
			return false;
		}
		if (parsed.matchId != matchId) {
			if (error) *error = "admission names match " + parsed.matchId;
			return false;
		}
		if (sealedHex.empty() || sealedHex.size() / 2 > c_MaxAdmissionBytes || !FromHex(sealedHex, parsed.sealed)) {
			if (error) *error = "admission carries no readable sealed export";
			return false;
		}
		parsed.path = path;
		out = std::move(parsed);
		return true;
	}

	std::vector<AutosaveDescriptor> AutosaveStore::ListResumable(const std::filesystem::path& directory) {
		std::vector<std::string> matchIds;
		std::error_code status;
		for (const auto& entry: std::filesystem::directory_iterator(directory, status)) {
			if (entry.is_symlink() || !entry.is_regular_file() || entry.path().extension() != c_AdmissionExtension) continue;
			const std::string matchId = entry.path().stem().string();
			if (ValidMatchId(matchId)) matchIds.push_back(matchId);
		}
		std::vector<AutosaveDescriptor> resumable;
		for (const std::string& matchId: matchIds) {
			for (AutosaveDescriptor& descriptor: ListRestorable(directory, matchId)) {
				// The newest checkpoint of this match that a restart can actually reopen.
				if (!descriptor.resumable) continue;
				resumable.push_back(std::move(descriptor));
				break;
			}
		}
		// Across matches the newer match is the one written more recently, not the one on a higher tick.
		std::sort(resumable.begin(), resumable.end(), [](const AutosaveDescriptor& left, const AutosaveDescriptor& right) {
			std::error_code ignored;
			return std::filesystem::last_write_time(left.path, ignored) > std::filesystem::last_write_time(right.path, ignored);
		});
		return resumable;
	}

	std::vector<AutosaveDescriptor> AutosaveStore::ListResumable() {
		return ListResumable(Directory());
	}

	std::vector<AutosaveDescriptor> AutosaveStore::ListRestorable(const std::filesystem::path& directory, const std::string& matchId) {
		std::vector<AutosaveDescriptor> restorable;
		if (!ValidMatchId(matchId)) return restorable;
		std::error_code status;
		for (const auto& entry: std::filesystem::directory_iterator(directory, status)) {
			uint64_t tick = 0;
			if (entry.is_symlink() || !entry.is_regular_file() || !ParseArchiveName(entry.path().filename().string(), matchId, tick)) continue;
			AutosaveDescriptor descriptor;
			if (Validate(entry.path(), descriptor)) restorable.push_back(std::move(descriptor));
		}
		std::sort(restorable.begin(), restorable.end(), [](const AutosaveDescriptor& left, const AutosaveDescriptor& right) {
			return left.savedTick > right.savedTick;
		});
		return restorable;
	}

	std::vector<AutosaveDescriptor> AutosaveStore::ListRestorable(const std::string& matchId) {
		return ListRestorable(Directory(), matchId);
	}

	std::optional<AutosaveDescriptor> AutosaveStore::NewestRestorable(const std::filesystem::path& directory, const std::string& matchId) {
		std::vector<AutosaveDescriptor> restorable = ListRestorable(directory, matchId);
		if (restorable.empty()) return std::nullopt;
		return std::move(restorable.front());
	}

	std::optional<AutosaveDescriptor> AutosaveStore::NewestRestorable(const std::string& matchId) {
		return NewestRestorable(Directory(), matchId);
	}

	std::optional<AutosaveDescriptor> AutosaveStore::Find(const std::filesystem::path& directory, const std::string& matchId, uint64_t tick, std::string* error) {
		if (!ValidMatchId(matchId) || tick == 0) {
			if (error) *error = "no match id or tick";
			return std::nullopt;
		}
		AutosaveDescriptor descriptor;
		if (!Validate(ArchivePath(directory, matchId, tick), descriptor, error)) return std::nullopt;
		return descriptor;
	}

	std::optional<AutosaveDescriptor> AutosaveStore::Find(const std::string& matchId, uint64_t tick, std::string* error) {
		return Find(Directory(), matchId, tick, error);
	}

	std::vector<uint64_t> AutosaveStore::RetainedTicks(const std::vector<AutosaveCandidate>& newestFirst, uint64_t pinnedTick) {
		std::vector<uint64_t> kept;
		size_t restorableKept = 0;
		for (const AutosaveCandidate& candidate: newestFirst) {
			// The agreed rewind point is kept whatever its age, and even when it no longer reads: a transient
			// read failure must not destroy the checkpoint both sides are rejoining onto.
			if (pinnedTick != c_NoPinnedTick && candidate.tick == pinnedTick) {
				kept.push_back(candidate.tick);
			} else if (candidate.restorable && restorableKept < c_RetainedAutosaves) {
				kept.push_back(candidate.tick);
				++restorableKept;
			}
		}
		return kept;
	}

	size_t AutosaveStore::ApplyRetention(const std::filesystem::path& directory, const std::string& matchId, uint64_t pinnedTick) {
		if (!ValidMatchId(matchId)) return 0;
		std::vector<std::pair<uint64_t, std::filesystem::path>> held;
		std::error_code status;
		for (const auto& entry: std::filesystem::directory_iterator(directory, status)) {
			uint64_t tick = 0;
			if (entry.is_symlink() || !entry.is_regular_file() || !ParseArchiveName(entry.path().filename().string(), matchId, tick)) continue;
			held.emplace_back(tick, entry.path());
		}
		std::sort(held.begin(), held.end(), [](const auto& left, const auto& right) { return left.first > right.first; });
		std::vector<AutosaveCandidate> candidates;
		std::string pinnedRefusal;
		for (const auto& [tick, path]: held) {
			AutosaveDescriptor descriptor;
			std::string refusal;
			const bool restorable = Validate(path, descriptor, &refusal);
			if (!restorable && tick == pinnedTick) pinnedRefusal = refusal;
			candidates.push_back({tick, restorable});
		}
		const std::vector<uint64_t> kept = RetainedTicks(candidates, pinnedTick);
		if (!pinnedRefusal.empty()) {
			std::cout << "[autosave] pinned tick=" << pinnedTick << " kept but not restorable: " << pinnedRefusal << std::endl;
		}
		size_t removed = 0;
		for (const auto& [tick, path]: held) {
			if (std::find(kept.begin(), kept.end(), tick) != kept.end()) continue;
			std::error_code ignored;
			removed += std::filesystem::remove(path, ignored) ? 1 : 0;
			// A checkpoint's restart manifest belongs to that checkpoint and goes with it.
			std::filesystem::remove(ManifestPath(directory, matchId, tick), ignored);
		}
		// The admission file lives as long as any checkpoint of the match does.
		if (kept.empty()) {
			std::error_code ignored;
			std::filesystem::remove(AdmissionPath(directory, matchId), ignored);
		}
		return removed;
	}

	size_t AutosaveStore::ApplyRetention(const std::string& matchId, uint64_t pinnedTick) {
		return ApplyRetention(Directory(), matchId, pinnedTick);
	}

	void AutosaveStore::NoteValidated(const AutosaveDescriptor& descriptor) {
		if (descriptor.matchId.empty() || descriptor.savedTick == 0) return;
		std::lock_guard<std::mutex> lock(s_ValidatedMutex);
		// A healed round can republish a tick it already wrote, so only an older tick of the same match loses.
		if (s_Validated.matchId == descriptor.matchId && descriptor.savedTick < s_Validated.savedTick) return;
		s_Validated = descriptor;
	}

	std::optional<AutosaveDescriptor> AutosaveStore::NewestValidated(const std::string& matchId) {
		std::lock_guard<std::mutex> lock(s_ValidatedMutex);
		if (matchId.empty() || s_Validated.matchId != matchId) return std::nullopt;
		std::error_code status;
		if (!std::filesystem::is_regular_file(s_Validated.path, status)) return std::nullopt;
		return s_Validated;
	}

	bool AutosaveStore::RunSelfTest(const std::string& matchId) {
		constexpr const char* Tag = "[autosave-store-selftest]";
		const std::filesystem::path scratch = Directory() / "selftest";
		std::error_code ignored;
		std::filesystem::remove_all(scratch, ignored);
		const std::vector<AutosaveDescriptor> held = ListRestorable(Directory(), matchId);
		if (held.size() < 2) {
			std::cout << Tag << " FAIL match=" << matchId << " restorable=" << held.size() << " (two checkpoints are needed)" << std::endl;
			return false;
		}
		std::filesystem::create_directories(scratch, ignored);
		for (const AutosaveDescriptor& descriptor: held) {
			std::filesystem::copy_file(descriptor.path, scratch / descriptor.path.filename(), std::filesystem::copy_options::overwrite_existing, ignored);
		}
		const std::vector<AutosaveDescriptor> copied = ListRestorable(scratch, matchId);
		const bool sameSet = copied.size() == held.size() &&
		                     std::equal(copied.begin(), copied.end(), held.begin(), [](const auto& left, const auto& right) { return left.savedTick == right.savedTick; });

		const std::filesystem::path torn = ArchivePath(scratch, matchId, held.front().savedTick);
		std::filesystem::resize_file(torn, std::filesystem::file_size(torn, ignored) / 2, ignored);
		AutosaveDescriptor refused;
		std::string reason;
		const bool tornRefused = !Validate(torn, refused, &reason);
		const std::optional<AutosaveDescriptor> picked = NewestRestorable(scratch, matchId);
		const bool skippedTorn = picked.has_value() && picked->savedTick == held[1].savedTick;

		const uint64_t pinned = held.back().savedTick;
		const size_t removed = ApplyRetention(scratch, matchId, pinned);
		const std::vector<AutosaveDescriptor> kept = ListRestorable(scratch, matchId);
		const bool tornDropped = std::none_of(kept.begin(), kept.end(), [&](const auto& entry) { return entry.savedTick == held.front().savedTick; });
		const bool pinnedKept = std::any_of(kept.begin(), kept.end(), [&](const auto& entry) { return entry.savedTick == pinned; });
		std::filesystem::remove_all(scratch, ignored);

		// The policy on a set a live directory never holds: the pin sits outside the newest window, so only
		// the pin can keep it, and a pinned checkpoint that no longer reads is kept where an unpinned one goes.
		static_assert(c_RetainedAutosaves == 3, "the kept ticks below are named by hand");
		const std::vector<AutosaveCandidate> synthetic = {{500, true}, {400, true}, {300, true}, {200, true}, {100, true}};
		const bool retentionWindow = RetainedTicks(synthetic, c_NoPinnedTick) == std::vector<uint64_t>{500, 400, 300};
		const bool pinOutsideWindow = RetainedTicks(synthetic, 100) == std::vector<uint64_t>{500, 400, 300, 100};
		const std::vector<AutosaveCandidate> unreadable = {{500, true}, {400, false}, {300, true}, {200, true}, {100, false}};
		const bool unreadablePinKept = RetainedTicks(unreadable, 100) == std::vector<uint64_t>{500, 300, 200, 100};

		const bool passed = sameSet && tornRefused && skippedTorn && removed >= 1 && tornDropped && pinnedKept &&
		                    retentionWindow && pinOutsideWindow && unreadablePinKept;
		std::cout << Tag << (passed ? " PASS" : " FAIL") << " match=" << matchId << " restorable=" << held.size()
		          << " same_set=" << sameSet << " torn_refused=" << tornRefused << " (" << reason << ")"
		          << " skipped_torn=" << skippedTorn << " removed=" << removed << " torn_dropped=" << tornDropped
		          << " pinned_kept=" << pinnedKept << " retention_window=" << retentionWindow
		          << " pin_outside_window=" << pinOutsideWindow << " unreadable_pin_kept=" << unreadablePinKept << std::endl;
		return passed;
	}
} // namespace RTE
