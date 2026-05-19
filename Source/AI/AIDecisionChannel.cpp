#include "AIDecisionChannel.h"

#include "SimChecksum.h"
#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cstring>

namespace RTE {

	AIDecisionChannel::AIDecisionChannel() {
		// Index 0 is reserved for "no string" / empty.
		m_Strings.emplace_back("");
		m_StringIndex.emplace("", static_cast<StringId>(0));
	}

	AIDecisionChannel::~AIDecisionChannel() = default;

	void AIDecisionChannel::Reset() {
		{
			std::lock_guard<std::mutex> lock(m_EventMutex);
			m_Events.clear();
		}
		{
			std::lock_guard<std::mutex> lock(m_HistoryMutex);
			m_History.clear();
		}
		{
			std::lock_guard<std::mutex> lock(m_StringMutex);
			m_Strings.clear();
			m_StringIndex.clear();
			m_Strings.emplace_back("");
			m_StringIndex.emplace("", static_cast<StringId>(0));
		}
		m_CurrentTick.store(0, std::memory_order_relaxed);
		m_Sequence.store(0, std::memory_order_relaxed);
	}

	AIDecisionChannel::StringId AIDecisionChannel::Intern(std::string_view s) {
		if (s.empty()) {
			return kNoString;
		}
		std::lock_guard<std::mutex> lock(m_StringMutex);
		std::string key(s);
		auto it = m_StringIndex.find(key);
		if (it != m_StringIndex.end()) {
			return it->second;
		}
		// Cap at uint16_t max minus a small margin.
		if (m_Strings.size() >= (static_cast<size_t>(UINT16_MAX) - 8)) {
			// Out of intern slots; return a stable "overflow" id at the boundary.
			return static_cast<StringId>(UINT16_MAX - 1);
		}
		const auto id = static_cast<StringId>(m_Strings.size());
		m_Strings.emplace_back(std::move(key));
		m_StringIndex.emplace(m_Strings.back(), id);
		return id;
	}

	std::string_view AIDecisionChannel::GetString(StringId id) const {
		std::lock_guard<std::mutex> lock(m_StringMutex);
		if (id >= m_Strings.size()) {
			return {};
		}
		return m_Strings[id];
	}

	void AIDecisionChannel::Emit(int actor_id, Layer layer, StringId type, StringId chosen, StringId reason,
	                             int target_actor_id, float target_x, float target_y) {
		ZoneScopedN("AIDecisionChannel::Emit");
		Event e;
		e.tick = m_CurrentTick.load(std::memory_order_relaxed);
		e.actor_id = actor_id;
		e.layer = layer;
		e.type = type;
		e.chosen = chosen;
		e.reason = reason;
		e.target_actor_id = target_actor_id;
		e.target_x = target_x;
		e.target_y = target_y;
		e.sequence = m_Sequence.fetch_add(1, std::memory_order_relaxed);

		{
			std::lock_guard<std::mutex> lock(m_EventMutex);
			m_Events.push_back(e);
		}
		{
			std::lock_guard<std::mutex> lock(m_HistoryMutex);
			auto& dq = m_History[actor_id];
			dq.push_back(e);
			while (dq.size() > kPerActorHistory) {
				dq.pop_front();
			}
		}
	}

	void AIDecisionChannel::EmitS(int actor_id, Layer layer, std::string_view type, std::string_view chosen,
	                              std::string_view reason, int target_actor_id, float target_x, float target_y) {
		const StringId t = Intern(type);
		const StringId c = Intern(chosen);
		const StringId r = Intern(reason);
		Emit(actor_id, layer, t, c, r, target_actor_id, target_x, target_y);
	}

	size_t AIDecisionChannel::Drain(std::vector<Event>& out) {
		ZoneScopedN("AIDecisionChannel::Drain");
		out.clear();
		{
			std::lock_guard<std::mutex> lock(m_EventMutex);
			out.reserve(m_Events.size());
			for (const auto& e: m_Events) {
				out.push_back(e);
			}
			m_Events.clear();
		}
		// M1 Block F — content-based sort.
		//
		// Pre-Block-F this sorted by (actor_id, tick, sequence). The `sequence`
		// field is a process-lifetime `std::atomic<uint32_t>` incremented by
		// fetch_add on every emit, so when two threads emit at the same
		// (actor_id, tick) the sequence assignment is race-order-dependent.
		// That made `sequence` a non-deterministic tiebreaker — across same-
		// seed runs the same logical event could get sequence 5 in run 1 and
		// 7 in run 2, and after the sort the two events would land in
		// different orders → the `decisions` subsystem's per-tick hash bytes
		// would differ even when the underlying sim is byte-identical.
		//
		// The fix: tiebreak on the event's content (layer, type, chosen,
		// target identification, target position) instead of the racy
		// sequence number. Content is deterministic; if two emits with
		// identical content land at the same (actor_id, tick), their
		// relative order doesn't affect any downstream consumer.
		std::sort(out.begin(), out.end(), [this](const Event& a, const Event& b) {
			if (a.actor_id != b.actor_id) return a.actor_id < b.actor_id;
			if (a.tick != b.tick) return a.tick < b.tick;
			if (a.layer != b.layer) return a.layer < b.layer;
			// Compare interned strings by content, not StringId — see
			// AIDecisionChannel::FeedToChecksum's header comment for why
			// the StringId values themselves are non-deterministic.
			const auto sa_type = GetString(a.type);
			const auto sb_type = GetString(b.type);
			if (sa_type != sb_type) return sa_type < sb_type;
			const auto sa_chosen = GetString(a.chosen);
			const auto sb_chosen = GetString(b.chosen);
			if (sa_chosen != sb_chosen) return sa_chosen < sb_chosen;
			const auto sa_reason = GetString(a.reason);
			const auto sb_reason = GetString(b.reason);
			if (sa_reason != sb_reason) return sa_reason < sb_reason;
			if (a.target_actor_id != b.target_actor_id) return a.target_actor_id < b.target_actor_id;
			if (a.target_x != b.target_x) return a.target_x < b.target_x;
			return a.target_y < b.target_y;
		});
		return out.size();
	}

	void AIDecisionChannel::FeedToChecksum(const std::vector<Event>& events, const char* subsystemName) const {
		ZoneScopedN("AIDecisionChannel::FeedToChecksum");
		for (const auto& e: events) {
			const uint64_t tick = e.tick;
			g_SimChecksum.Update(subsystemName, &tick, sizeof(tick));
			const int32_t actor_id = static_cast<int32_t>(e.actor_id);
			g_SimChecksum.Update(subsystemName, &actor_id, sizeof(actor_id));
			const uint8_t layer = static_cast<uint8_t>(e.layer);
			g_SimChecksum.Update(subsystemName, &layer, sizeof(layer));

			auto feedString = [this, subsystemName](StringId id) {
				const std::string_view s = GetString(id);
				const uint32_t len = static_cast<uint32_t>(s.size());
				g_SimChecksum.Update(subsystemName, &len, sizeof(len));
				if (len > 0) {
					g_SimChecksum.Update(subsystemName, s.data(), s.size());
				}
			};
			feedString(e.type);
			feedString(e.chosen);
			feedString(e.reason);

			const int32_t target_actor_id = static_cast<int32_t>(e.target_actor_id);
			g_SimChecksum.Update(subsystemName, &target_actor_id, sizeof(target_actor_id));
			const float target_x = e.target_x;
			g_SimChecksum.Update(subsystemName, &target_x, sizeof(target_x));
			const float target_y = e.target_y;
			g_SimChecksum.Update(subsystemName, &target_y, sizeof(target_y));
			// Note: e.sequence intentionally omitted — see header comment.
		}
	}

	void AIDecisionChannel::PeekRecent(int actor_id, size_t maxEvents, std::vector<Event>& out) const {
		out.clear();
		std::lock_guard<std::mutex> lock(m_HistoryMutex);
		auto it = m_History.find(actor_id);
		if (it == m_History.end()) {
			return;
		}
		const auto& dq = it->second;
		const size_t take = std::min(maxEvents, dq.size());
		out.reserve(take);
		// Tick-descending: walk back from the end.
		for (auto rit = dq.rbegin(); rit != dq.rend() && out.size() < take; ++rit) {
			out.push_back(*rit);
		}
	}

	const char* AIDecisionChannel::LayerName(Layer layer) {
		switch (layer) {
			case Layer::Reflex: return "reflex";
			case Layer::Locomotion: return "locomotion";
			case Layer::Squad: return "squad";
			case Layer::Decision: return "decision";
			default: return "unknown";
		}
	}

	AIDecisionChannel::Layer AIDecisionChannel::LayerFromName(std::string_view name) {
		auto eq = [&name](const char* lit) {
			const auto len = std::strlen(lit);
			if (name.size() != len) return false;
			for (size_t i = 0; i < len; ++i) {
				const char a = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] + 32) : name[i];
				if (a != lit[i]) return false;
			}
			return true;
		};
		if (eq("reflex")) return Layer::Reflex;
		if (eq("locomotion")) return Layer::Locomotion;
		if (eq("squad")) return Layer::Squad;
		return Layer::Decision;
	}

} // namespace RTE
