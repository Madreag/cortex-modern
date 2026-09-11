#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace RTE {

	/// What a preview played early, so the canonical emission adopts it instead of starting a second one.
	/// Presentation state only: never hashed, saved, snapshotted or sent.
	class PreviewEventLedger {
	public:
		enum Kind : uint8_t { Sound = 0,
			                  PostEffect = 1 };

		struct Key {
			uint8_t kind = Sound;
			uint64_t emitterUID = 0;
			uint64_t assetIdentity = 0; //!< The checkpoint identity of the emitting container, 0 when it names nothing canonical.
			uint64_t presetHash = 0;
			uint64_t tick = 0; //!< The predicted absolute sim tick.
			uint32_t seq = 0;  //!< Emission order among identical events in that tick.
		};

		struct Counters {
			uint64_t playedAtPreview = 0;
			uint64_t adoptedAtCommit = 0;
			uint64_t expired = 0;
			uint64_t retimed = 0;
		};

		/// One physical voice start by a previewed emitter, for the latency report.
		struct SoundStart {
			uint64_t committedTick = 0;
			uint64_t eventTick = 0;
			uint64_t emitterUID = 0;
			uint32_t seq = 0;
			bool predicted = false;
		};

		/// Opens the ledger for one preview: the tick the canonical world is on, the sound identity cursor
		/// at that moment, and the emitter roots whose events may play early.
		static void Arm(uint64_t committedTick, uint64_t soundIdentityCursor, std::vector<uint64_t> emitters);
		static void AddPreviewedEmitters(const std::vector<uint64_t>& emitters);
		static void Disarm();
		static bool IsArmed() { return s_Armed; }
		static uint64_t GetArmCount() { return s_ArmCount; }
		static bool IsPreviewedEmitter(uint64_t emitterUID);
		/// The tick the canonical world is on, which inside a preview is the tick it started from.
		static uint64_t CommittedTick();
		/// An identity allocated inside the preview names nothing canonical, so it is not part of the key.
		static uint64_t StableAssetIdentity(uint64_t identity);

		/// Numbers this emission inside its tick; a preview and its canonical tick number identically.
		static Key NextKey(uint8_t kind, uint64_t emitterUID, uint64_t assetIdentity, uint64_t presetHash, uint64_t tick);
		/// True when an earlier preview already played this event and it must not play twice.
		static bool AlreadyPlayed(const Key& key);
		static void Insert(const Key& key, std::vector<int> voices);
		/// Takes the entry a canonical emission matches, with the voices it must adopt.
		static bool Consume(const Key& key, std::vector<int>& voices);
		/// Drops the entries the committed tick has passed; once per sim tick.
		static void ExpireForTick(uint64_t committedTick);
		/// Drops everything on a match teardown or a resync; live entries count as mispredictions.
		static void Clear();

		static const Counters& GetCounters() { return s_Counters; }
		static void NoteSoundStart(uint64_t committedTick, const Key& key, bool predicted);
		static const std::vector<SoundStart>& GetSoundStarts() { return s_SoundStarts; }
		static uint64_t GetSoundStartCount() { return s_SoundStartCount; }
		static size_t GetLiveEntryCount() { return s_Entries.size(); }
		/// One line of counters for the match report; empty when nothing was ever predicted.
		static std::string Describe();
		static bool TraceEnabled();
		/// Insert, match, expiry, numbering and the one-tick tolerance, with no engine around them.
		static bool RunSelfTest();

	private:
		struct Entry {
			Key key;
			std::vector<int> voices;
			uint64_t expiresAfterTick = 0;
		};
		using Tuple = std::tuple<uint8_t, uint64_t, uint64_t, uint64_t, uint64_t>;
		static Tuple TupleOf(const Key& key);
		static bool SameEvent(const Key& first, const Key& second);
		static int Find(const Key& key, bool& retimed);
		static void Retire(Entry& entry);
		static void ResetForSelfTest();

		static std::vector<Entry> s_Entries;
		static std::vector<uint64_t> s_PreviewedEmitters;
		static std::map<Tuple, uint32_t> s_PreviewSeq;
		static std::map<Tuple, uint32_t> s_CanonicalSeq;
		static std::vector<SoundStart> s_SoundStarts;
		static Counters s_Counters;
		static uint64_t s_SoundStartCount;
		static uint64_t s_CommittedTick;
		static uint64_t s_IdentityCursor;
		static uint64_t s_CanonicalSeqTick;
		static uint64_t s_ArmCount;
		static bool s_Armed;
	};
} // namespace RTE
