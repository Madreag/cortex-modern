#include "PreviewEventLedger.h"

#include "AudioMan.h"
#include "TimerMan.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace RTE {

	std::vector<PreviewEventLedger::Entry> PreviewEventLedger::s_Entries;
	std::vector<uint64_t> PreviewEventLedger::s_PreviewedEmitters;
	std::map<PreviewEventLedger::Tuple, uint32_t> PreviewEventLedger::s_PreviewSeq;
	std::map<PreviewEventLedger::Tuple, uint32_t> PreviewEventLedger::s_CanonicalSeq;
	std::vector<PreviewEventLedger::EventStart> PreviewEventLedger::s_EventStarts;
	PreviewEventLedger::Counters PreviewEventLedger::s_Counters;
	uint64_t PreviewEventLedger::s_EventStartCount = 0;
	uint64_t PreviewEventLedger::s_CommittedTick = 0;
	uint64_t PreviewEventLedger::s_IdentityCursor = 0;
	uint64_t PreviewEventLedger::s_CanonicalSeqTick = 0;
	uint64_t PreviewEventLedger::s_ArmCount = 0;
	bool PreviewEventLedger::s_Armed = false;

	namespace {
		constexpr size_t c_MaxRecordedEventStarts = 64;
	}

	PreviewEventLedger::Tuple PreviewEventLedger::TupleOf(const Key& key) {
		return {key.kind, key.emitterUID, key.assetIdentity, key.presetHash, key.tick};
	}

	bool PreviewEventLedger::SameEvent(const Key& first, const Key& second) {
		// A transient emitter's identity is freshly allocated on each side, so a zero means "not comparable".
		const bool asset = first.assetIdentity == second.assetIdentity || !first.assetIdentity || !second.assetIdentity;
		return first.kind == second.kind && first.emitterUID == second.emitterUID && first.presetHash == second.presetHash && first.seq == second.seq && asset;
	}

	int PreviewEventLedger::Find(const Key& key, bool& retimed) {
		retimed = false;
		for (size_t index = 0; index < s_Entries.size(); ++index) {
			if (s_Entries[index].key.tick == key.tick && SameEvent(s_Entries[index].key, key)) return static_cast<int>(index);
		}
		int candidate = -1;
		for (size_t index = 0; index < s_Entries.size(); ++index) {
			const uint64_t tick = s_Entries[index].key.tick;
			if ((tick == key.tick + 1 || tick + 1 == key.tick) && SameEvent(s_Entries[index].key, key)) {
				if (candidate >= 0) return -1;
				candidate = static_cast<int>(index);
			}
		}
		retimed = candidate >= 0;
		return candidate;
	}

	void PreviewEventLedger::Retire(Entry& entry) {
		for (int voice: entry.voices) g_AudioMan.RetirePredictedVoice(voice);
		entry.voices.clear();
	}

	bool PreviewEventLedger::TraceEnabled() {
		static const bool enabled = std::getenv("CC_TRACE_PREVIEW_EVENT") != nullptr;
		return enabled;
	}

	void PreviewEventLedger::Arm(uint64_t committedTick, uint64_t soundIdentityCursor, std::vector<uint64_t> emitters) {
		s_Armed = true;
		s_CommittedTick = committedTick;
		s_IdentityCursor = soundIdentityCursor;
		s_PreviewedEmitters = std::move(emitters);
		s_PreviewSeq.clear();
		++s_ArmCount;
	}

	void PreviewEventLedger::AddPreviewedEmitters(const std::vector<uint64_t>& emitters) {
		for (uint64_t emitter: emitters) {
			if (emitter && !IsPreviewedEmitter(emitter)) s_PreviewedEmitters.push_back(emitter);
		}
	}

	void PreviewEventLedger::Disarm() {
		s_Armed = false;
		s_PreviewSeq.clear();
	}

	bool PreviewEventLedger::IsPreviewedEmitter(uint64_t emitterUID) {
		return emitterUID && std::find(s_PreviewedEmitters.begin(), s_PreviewedEmitters.end(), emitterUID) != s_PreviewedEmitters.end();
	}

	uint64_t PreviewEventLedger::CommittedTick() {
		return s_Armed ? s_CommittedTick : static_cast<uint64_t>(g_TimerMan.GetSimUpdateCount());
	}

	uint64_t PreviewEventLedger::StableAssetIdentity(uint64_t identity) {
		return s_Armed && identity > s_IdentityCursor ? 0 : identity;
	}

	PreviewEventLedger::Key PreviewEventLedger::NextKey(uint8_t kind, uint64_t emitterUID, uint64_t assetIdentity, uint64_t presetHash, uint64_t tick) {
		Key key;
		key.kind = kind;
		key.emitterUID = emitterUID;
		key.assetIdentity = assetIdentity;
		key.presetHash = presetHash;
		key.tick = tick;
		std::map<Tuple, uint32_t>& counters = s_Armed ? s_PreviewSeq : s_CanonicalSeq;
		if (!s_Armed && tick != s_CanonicalSeqTick) {
			s_CanonicalSeq.clear();
			s_CanonicalSeqTick = tick;
		}
		const Tuple tuple = TupleOf(key);
		const auto found = counters.find(tuple);
		key.seq = found == counters.end() ? 0 : found->second + 1;
		counters[tuple] = key.seq;
		return key;
	}

	bool PreviewEventLedger::AlreadyPlayed(const Key& key) {
		bool retimed = false;
		return Find(key, retimed) >= 0;
	}

	void PreviewEventLedger::Insert(const Key& key, std::vector<int> voices) {
		s_Entries.push_back({key, std::move(voices), key.tick + 1});
		++s_Counters.playedAtPreview;
		if (TraceEnabled()) {
			std::cout << "[preview-event] played kind=" << static_cast<int>(key.kind) << " uid=" << key.emitterUID << " asset=" << key.assetIdentity << " preset=" << key.presetHash
			          << " tick=" << key.tick << " seq=" << key.seq << " committed=" << CommittedTick() << " voices=" << s_Entries.back().voices.size() << std::endl;
		}
	}

	bool PreviewEventLedger::Consume(const Key& key, std::vector<int>& voices) {
		if (s_Entries.empty()) return false;
		bool retimed = false;
		const int index = Find(key, retimed);
		if (index < 0) return false;
		voices = std::move(s_Entries[static_cast<size_t>(index)].voices);
		s_Entries.erase(s_Entries.begin() + index);
		++s_Counters.adoptedAtCommit;
		if (retimed) ++s_Counters.retimed;
		if (TraceEnabled()) {
			std::cout << "[preview-event] adopted kind=" << static_cast<int>(key.kind) << " uid=" << key.emitterUID << " tick=" << key.tick << " seq=" << key.seq
			          << " retimed=" << (retimed ? 1 : 0) << " voices=" << voices.size() << std::endl;
		}
		return true;
	}

	void PreviewEventLedger::ExpireForTick(uint64_t committedTick) {
		for (auto entry = s_Entries.begin(); entry != s_Entries.end();) {
			if (committedTick <= entry->expiresAfterTick) {
				++entry;
				continue;
			}
			Retire(*entry);
			++s_Counters.expired;
			if (TraceEnabled()) {
				std::cout << "[preview-event] expired kind=" << static_cast<int>(entry->key.kind) << " uid=" << entry->key.emitterUID << " tick=" << entry->key.tick << " seq=" << entry->key.seq << " at=" << committedTick << std::endl;
			}
			entry = s_Entries.erase(entry);
		}
	}

	void PreviewEventLedger::Clear() {
		for (Entry& entry: s_Entries) {
			Retire(entry);
			++s_Counters.expired;
		}
		s_Entries.clear();
		s_PreviewedEmitters.clear();
		s_PreviewSeq.clear();
		s_CanonicalSeq.clear();
		s_Armed = false;
	}

	void PreviewEventLedger::NoteEventStart(uint64_t committedTick, const Key& key, bool predicted) {
		++s_EventStartCount;
		if (s_EventStarts.size() < c_MaxRecordedEventStarts) s_EventStarts.push_back({committedTick, key.tick, key.emitterUID, key.seq, key.kind, predicted});
	}

	std::string PreviewEventLedger::Describe() {
		if (!s_Counters.playedAtPreview && !s_EventStartCount) return "";
		return "events_played_at_preview=" + std::to_string(s_Counters.playedAtPreview) + " events_suppressed_at_commit=" + std::to_string(s_Counters.adoptedAtCommit) +
		       " events_expired=" + std::to_string(s_Counters.expired) + " events_retimed=" + std::to_string(s_Counters.retimed);
	}

	void PreviewEventLedger::ResetForSelfTest() {
		s_Entries.clear();
		s_PreviewedEmitters.clear();
		s_PreviewSeq.clear();
		s_CanonicalSeq.clear();
		s_EventStarts.clear();
		s_Counters = {};
		s_EventStartCount = 0;
		s_CommittedTick = 0;
		s_IdentityCursor = 0;
		s_CanonicalSeqTick = 0;
		s_ArmCount = 0;
		s_Armed = false;
	}

	bool PreviewEventLedger::RunSelfTest() {
		bool passed = true;
		const auto check = [&passed](const char* name, bool ok, const std::string& detail = std::string()) {
			std::cout << "[preview-event-selftest] " << (ok ? "PASS " : "FAIL ") << name << (detail.empty() ? "" : " " + detail) << std::endl;
			passed = passed && ok;
		};
		const auto key = [](uint64_t uid, uint64_t asset, uint64_t preset, uint64_t tick, uint32_t seq) {
			Key made;
			made.emitterUID = uid;
			made.assetIdentity = asset;
			made.presetHash = preset;
			made.tick = tick;
			made.seq = seq;
			return made;
		};

		ResetForSelfTest();
		Arm(100, 500, {7});
		check("armed_publishes_emitters", IsArmed() && IsPreviewedEmitter(7) && !IsPreviewedEmitter(8) && CommittedTick() == 100);
		check("identity_past_the_cursor_is_transient", StableAssetIdentity(499) == 499 && StableAssetIdentity(501) == 0);
		const Key first = NextKey(Sound, 7, 42, 9, 104);
		const Key second = NextKey(Sound, 7, 42, 9, 104);
		check("seq_numbers_identical_events", first.seq == 0 && second.seq == 1);
		Insert(first, {});
		Insert(second, {});
		check("insert_counts_and_dedups", GetCounters().playedAtPreview == 2 && AlreadyPlayed(first) && AlreadyPlayed(second) && !AlreadyPlayed(key(7, 42, 9, 104, 2)));
		Disarm();
		const Key canonicalFirst = NextKey(Sound, 7, 42, 9, 104);
		std::vector<int> voices;
		check("canonical_numbering_matches_the_preview", canonicalFirst.seq == 0 && Consume(canonicalFirst, voices) && GetCounters().adoptedAtCommit == 1);
		check("an_event_is_adopted_once", !Consume(canonicalFirst, voices));
		ExpireForTick(105);
		check("the_window_holds_for_one_tick", GetLiveEntryCount() == 1 && GetCounters().expired == 0);
		ExpireForTick(106);
		check("an_unclaimed_event_expires", GetLiveEntryCount() == 0 && GetCounters().expired == 1);
		check("counters_balance", GetCounters().playedAtPreview == GetCounters().adoptedAtCommit + GetCounters().expired);

		ResetForSelfTest();
		Arm(100, 500, {7});
		Insert(NextKey(Sound, 7, 42, 9, 104), {});
		Disarm();
		check("one_tick_slip_is_absorbed", Consume(key(7, 42, 9, 105, 0), voices) && GetCounters().retimed == 1);

		ResetForSelfTest();
		Arm(100, 500, {7});
		Insert(NextKey(Sound, 7, 42, 9, 104), {});
		Insert(NextKey(Sound, 7, 42, 9, 106), {});
		Disarm();
		check("an_ambiguous_slip_is_refused", !Consume(key(7, 42, 9, 105, 0), voices) && GetLiveEntryCount() == 2);
		check("a_two_tick_slip_is_refused", !Consume(key(7, 42, 9, 108, 0), voices));

		ResetForSelfTest();
		Arm(100, 500, {7});
		Insert(NextKey(Sound, 7, 0, 9, 104), {});
		Disarm();
		check("a_transient_emitter_matches_on_its_preset", Consume(key(7, 77, 9, 104, 0), voices));

		ResetForSelfTest();
		Arm(100, 500, {7});
		Insert(NextKey(Sound, 7, 42, 9, 104), {});
		Insert(NextKey(PostEffect, 7, 42, 9, 104), {});
		Disarm();
		Key postKey = key(7, 42, 9, 104, 0);
		postKey.kind = PostEffect;
		check("a_sound_does_not_adopt_a_post_effect", Consume(key(7, 42, 9, 104, 0), voices) && GetLiveEntryCount() == 1 && s_Entries.front().key.kind == PostEffect);
		check("a_post_effect_is_its_own_event", Consume(postKey, voices) && GetLiveEntryCount() == 0);

		ResetForSelfTest();
		Arm(100, 500, {7});
		Insert(NextKey(Sound, 7, 42, 9, 104), {});
		Disarm();
		Clear();
		check("clear_drops_and_counts_the_rest", GetLiveEntryCount() == 0 && GetCounters().expired == 1 && GetCounters().playedAtPreview == GetCounters().adoptedAtCommit + GetCounters().expired);
		check("clear_disarms_and_drops_the_emitters", !IsArmed() && !IsPreviewedEmitter(7));
		ResetForSelfTest();
		std::cout << "[preview-event-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
		return passed;
	}
} // namespace RTE
