#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RTE {

	/// What a restore needs from an autosave before its world is read: who made it, which committed tick it
	/// stands on, and the content the peers agreed on when they made it. Every field is agreed by the
	/// whole match, so the descriptor of a given tick is identical on every peer: nothing per-machine
	/// belongs here, where the snapshot comparer reads it beside the world.
	struct AutosaveDescriptor {
		int schema = 0;
		std::string matchId;
		uint64_t sessionId = 0;
		uint64_t roundId = 0;
		uint64_t savedTick = 0;
		long long simTimeTicks = 0;
		uint32_t intervalSeconds = 0;
		std::string gameVersion;
		std::string buildId;
		std::string deterministicConfigHash;
		std::string moduleManifestHash;
		std::string sessionIdentityHash;
		std::string worldStructureHash;
		std::string activityPreset;
		std::string scenePreset;
		std::filesystem::path path; //!< Where it was found; never part of the stored text.
		/// Whether this checkpoint's restart manifest reads beside it, so a restarted host can reopen the
		/// lobby it was written under. Derived from the files, never part of the stored text.
		bool resumable = false;
	};

	/// The lockstep state every peer of a match agrees on at a committed tick: who controls which actor,
	/// which of those handoffs belong to seats that dropped, how far each sender's commands have been
	/// applied, and the first owner transfer of the round. Every peer holds the same values at the same
	/// tick, so each peer stamps its own checkpoint with them and a restart hands every peer one state.
	struct AutosaveSideState {
		std::map<int64_t, uint8_t> controlOwners;
		std::map<int64_t, uint8_t> droppedControlOwners;
		std::map<uint8_t, uint64_t> appliedCommands;
		int64_t firstTransferUid = 0;
		bool operator==(const AutosaveSideState&) const = default;
	};

	/// What a restarted host needs beside the world to reopen the match the checkpoints were written
	/// under: the agreed configuration exactly as the peers hashed it, and who was playing it. The
	/// payload is opaque here - the network layer owns the lobby encoding - so the store keeps the bytes
	/// and the hash the peers agreed on and never interprets either.
	struct AutosaveManifest {
		int schema = 0;
		std::string matchId;
		uint64_t sessionId = 0;
		uint64_t roundId = 0;
		uint64_t savedTick = 0;
		long long simTimeTicks = 0;
		uint32_t intervalSeconds = 0;
		std::string configHash;    //!< Hex of the hash every peer acked for this configuration.
		std::string configPayload; //!< Hex of the exact lobby payload bytes that carried it.
		std::string activityPreset;
		std::string scenePreset;
		std::vector<std::string> peerNames;
		AutosaveSideState sideState; //!< The agreed lockstep state of that committed tick.
		std::filesystem::path path; //!< Where it was found; never part of the stored text.
	};

	/// The host's own admission state at rest: one live file per match, sealed for this install alone.
	/// Its plaintext is the network layer's; the store carries the bytes and the generation they belong to.
	struct AutosaveAdmission {
		int schema = 0;
		std::string matchId;
		uint64_t generation = 0; //!< Rises with every export, so an older file never replaces a newer one.
		std::vector<uint8_t> sealed;
		std::filesystem::path path;
	};

	/// What the match agreed on while it runs, stamped into every checkpoint it writes.
	struct AutosaveIdentity {
		uint64_t sessionId = 0;
		uint64_t roundId = 0;
		uint32_t intervalSeconds = 0;
		/// The agreed configuration, as the peers hashed it, so a restart can reopen this very lobby.
		/// Empty on a match whose configuration was never published (a local activity).
		std::string configHash;
		std::string configPayload;
		std::vector<std::string> peerNames;
		/// The agreed lockstep state of the tick being captured, read on the sim thread at the boundary.
		AutosaveSideState sideState;
		/// The agreed rewind point, read where retention runs rather than where the capture starts, so an
		/// anchor named while a capture is in flight still protects its archive.
		std::shared_ptr<const std::atomic<uint64_t>> pinnedTickSource;
		std::string buildId;
		std::string deterministicConfigHash;
		std::string moduleManifestHash;
		std::string sessionIdentityHash;
	};

	/// One checkpoint as the retention policy sees it: which tick it stands on and whether it still reads.
	struct AutosaveCandidate {
		uint64_t tick = 0;
		bool restorable = false;
	};

	/// The match checkpoint store: where autosaves live, what makes one restorable, and which ones are kept.
	/// Every peer of a match runs this same policy on the same file names, so a rejoin can name one archive.
	class AutosaveStore {
	public:
		/// A match keeps this many restorable checkpoints, newest first, plus a pinned one.
		static constexpr size_t c_RetainedAutosaves = 3;
		static constexpr int c_DescriptorSchema = 1;
		static constexpr size_t c_MaxMatchIdBytes = 64;
		static constexpr uint64_t c_NoPinnedTick = 0;
		static constexpr const char* c_DescriptorEntry = "Restore.ini";
		static constexpr const char* c_ArchiveExtension = ".ccsave";
		static constexpr const char* c_ManifestExtension = ".ccmanifest";
		static constexpr const char* c_AdmissionExtension = ".admission";
		static constexpr int c_ManifestSchema = 1;
		static constexpr int c_AdmissionSchema = 1;
		/// The host's sealed admission export is small by construction; anything larger is not one.
		static constexpr size_t c_MaxAdmissionBytes = 64U * 1024U;

		static std::filesystem::path Directory();
		static std::string ArchiveName(const std::string& matchId, uint64_t tick);
		static std::filesystem::path ArchivePath(const std::string& matchId, uint64_t tick);
		static std::filesystem::path ArchivePath(const std::filesystem::path& directory, const std::string& matchId, uint64_t tick);
		/// The restart manifest that stands beside one checkpoint, and the match's one live admission file.
		static std::filesystem::path ManifestPath(const std::filesystem::path& directory, const std::string& matchId, uint64_t tick);
		static std::filesystem::path AdmissionPath(const std::filesystem::path& directory, const std::string& matchId);
		/// A match id is hex digits and dashes only, so it can never escape the store's directory.
		static bool ValidMatchId(const std::string& matchId);
		/// Reads the tick out of "<matchId>-<tick>.ccsave"; false for any other name.
		static bool ParseArchiveName(const std::string& fileName, const std::string& matchId, uint64_t& outTick);

		static std::string WriteDescriptor(const AutosaveDescriptor& descriptor);
		static bool ParseDescriptor(const std::string& text, AutosaveDescriptor& out, std::string* error = nullptr);

		/// Whether the archive is restorable: every entry a restore reads is complete, a descriptor of a schema
		/// we know rides along, and the tick in the name, in the descriptor and in the world all agree.
		static bool Validate(const std::filesystem::path& path, AutosaveDescriptor& out, std::string* error = nullptr);
		/// This match's restorable checkpoints, newest tick first.
		static std::vector<AutosaveDescriptor> ListRestorable(const std::filesystem::path& directory, const std::string& matchId);
		static std::vector<AutosaveDescriptor> ListRestorable(const std::string& matchId);
		/// The checkpoint a rejoin rewinds to: the newest restorable one this peer holds.
		static std::optional<AutosaveDescriptor> NewestRestorable(const std::filesystem::path& directory, const std::string& matchId);
		static std::optional<AutosaveDescriptor> NewestRestorable(const std::string& matchId);
		/// One named checkpoint, validated. The error says why a held file is not restorable.
		static std::optional<AutosaveDescriptor> Find(const std::filesystem::path& directory, const std::string& matchId, uint64_t tick, std::string* error = nullptr);
		static std::optional<AutosaveDescriptor> Find(const std::string& matchId, uint64_t tick, std::string* error = nullptr);

		/// The retention policy itself, decided on the candidates alone: this match keeps its newest
		/// c_RetainedAutosaves restorable checkpoints plus the pinned one whatever its age, and loses
		/// everything else it wrote. A pinned checkpoint is kept even when it no longer reads, so one
		/// transient read failure cannot destroy the checkpoint both sides agreed to rewind to.
		/// @param newestFirst This match's checkpoints, newest tick first.
		/// @param pinnedTick The tick both sides agreed to rewind to, or c_NoPinnedTick.
		/// @return The ticks that are kept, newest first.
		static std::vector<uint64_t> RetainedTicks(const std::vector<AutosaveCandidate>& newestFirst, uint64_t pinnedTick);

		/// Applies the policy to this match's archives: what it does not keep is deleted, so what remains is
		/// exactly what a rejoin can use. Other matches' files and anything that is not one of this match's
		/// archives are left alone.
		/// @return How many archives were removed.
		static size_t ApplyRetention(const std::filesystem::path& directory, const std::string& matchId, uint64_t pinnedTick);
		static size_t ApplyRetention(const std::string& matchId, uint64_t pinnedTick);

		/// The one rendering of the agreed side state: the manifest is written with it and its hash is
		/// taken over it, so a peer's answer and the host's offer are compared over the same bytes.
		static std::string RenderSideState(const AutosaveSideState& state);
		static std::string WriteManifest(const AutosaveManifest& manifest);
		static bool ParseManifest(const std::string& text, AutosaveManifest& out, std::string* error = nullptr);
		/// Publishes a checkpoint's restart manifest: written to a temporary name and renamed, so a reader
		/// sees either the previous generation or this one. The archive is published first, so a manifest
		/// without its world never exists; an archive without a manifest is restorable but not resumable.
		static bool PublishManifest(const std::filesystem::path& directory, const AutosaveManifest& manifest, std::string* error = nullptr);
		static bool ReadManifest(const std::filesystem::path& path, AutosaveManifest& out, std::string* error = nullptr);
		static bool ReadManifest(const std::filesystem::path& directory, const std::string& matchId, uint64_t tick, AutosaveManifest& out, std::string* error = nullptr);

		/// Writes the match's one live admission file, the same atomic way. The bytes are already sealed
		/// for this install; the store never sees their plaintext and never chooses their key.
		static bool PublishAdmission(const std::filesystem::path& directory, const AutosaveAdmission& admission, std::string* error = nullptr);
		static bool ReadAdmission(const std::filesystem::path& directory, const std::string& matchId, AutosaveAdmission& out, std::string* error = nullptr);
		/// Removes a match's admission file once no checkpoint of that match is left to resume.
		/// @return Whether the match had no checkpoint left (whether or not a file was there to remove).
		static bool RemoveOrphanAdmission(const std::filesystem::path& directory, const std::string& matchId);

		/// The matches this install can restart: for each, the newest checkpoint whose manifest reads and
		/// whose admission file is present, newest checkpoint first.
		static std::vector<AutosaveDescriptor> ListResumable(const std::filesystem::path& directory);
		static std::vector<AutosaveDescriptor> ListResumable();

		/// Records a checkpoint this process published and proved restorable, so a heal can name the rewind
		/// point without reading every archive of the match again.
		static void NoteValidated(const AutosaveDescriptor& descriptor);
		/// The newest checkpoint of this match that this process published and validated, if it is still on
		/// disk. Costs one file status query, never an archive read.
		static std::optional<AutosaveDescriptor> NewestValidated(const std::string& matchId);

		/// Exercises the policy on copies of this match's own checkpoints: the retained set, a torn
		/// newest that must not be picked, and retention keeping the pinned rewind point.
		static bool RunSelfTest(const std::string& matchId);
	};
} // namespace RTE
