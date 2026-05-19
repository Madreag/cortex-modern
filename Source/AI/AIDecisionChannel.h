#pragma once

#include "Singleton.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define g_AIDecisionChannel AIDecisionChannel::Instance()

namespace RTE {

	/// Per-actor structured AI decision channel.
	///
	/// Captures decisions emitted by the AI Lua (`ai.emit(layer, type, chosen, reason, target)`) into a
	/// thread-safe ring buffer. Drained once per tick by `MovableMan::Update` and consumed by:
	///   - The metrics collector (headless trust-suite runs)
	///   - The in-game debug overlay
	///   - The per-tick state checksum (the `decisions` subsystem)
	///
	/// Strings are interned process-wide so the hot path is allocation-free after first sighting.
	/// Built determinism-friendly from line one: drain returns events sorted by (actor_id, tick, sequence).
	class AIDecisionChannel : public Singleton<AIDecisionChannel> {
		friend class Singleton<AIDecisionChannel>;

	public:
		enum class Layer : uint8_t {
			Reflex = 0,
			Locomotion,
			Squad,
			Decision,
			Count
		};

		/// String-id type. 0 is reserved for "none/empty".
		using StringId = uint16_t;
		static constexpr StringId kNoString = 0;

		struct Event {
			uint64_t tick = 0;
			int      actor_id = -1;
			Layer    layer = Layer::Decision;
			StringId type = kNoString;
			StringId chosen = kNoString;
			StringId reason = kNoString;
			int      target_actor_id = -1;
			float    target_x = 0.0f;
			float    target_y = 0.0f;
			uint32_t sequence = 0; //!< Tie-breaker for events emitted in the same tick. Deterministic.
		};

		AIDecisionChannel();
		~AIDecisionChannel();

		/// Initialize. No-op currently; kept for symmetry with the manager lifecycle convention.
		void Initialize() {}

		/// Reset and clear all state. Called from `Destroy` or test setups.
		void Reset();

		/// Destroy. Clears state.
		void Destroy() { Reset(); }

		/// Intern a string. Thread-safe. Returns the same id for the same string.
		/// An empty string is reserved as `kNoString`.
		StringId Intern(std::string_view s);

		/// Look up the string for an id. Thread-safe. Returns an empty view if id is unknown.
		std::string_view GetString(StringId id) const;

		/// Emit an event. Thread-safe; safe to call from the AI script thread pool.
		/// The current tick is recorded automatically; `sequence` is auto-assigned.
		void Emit(int actor_id, Layer layer, StringId type, StringId chosen, StringId reason,
		          int target_actor_id, float target_x, float target_y);

		/// Convenience: emit using string literals; interns on first sighting.
		void EmitS(int actor_id, Layer layer, std::string_view type, std::string_view chosen,
		           std::string_view reason, int target_actor_id, float target_x, float target_y);

		/// Drain all events into a flat vector, sorted by (actor_id, tick, sequence) for determinism.
		/// Drained events are removed from the channel. Returns the number of events drained.
		size_t Drain(std::vector<Event>& out);

		/// Peek at the most recent K events for an actor (for the debug overlay). Does not drain.
		/// Returned in tick-descending order.
		void PeekRecent(int actor_id, size_t maxEvents, std::vector<Event>& out) const;

		/// M1 Block F — feed a vector of (drained-and-sorted) events into a
		/// SimChecksum subsystem accumulator deterministically.
		///
		/// A naive `g_SimChecksum.Update("decisions", events.data(),
		/// events.size() * sizeof(Event))` leaks two non-deterministic things
		/// into the hash:
		///   (1) `Event::sequence` — a process-lifetime atomic counter,
		///       race-prone across threads.
		///   (2) `Event::type / chosen / reason` — `StringId`s assigned by
		///       insertion order into the intern table; the order itself is
		///       a function of which thread interned which string first, so
		///       same-content strings can land on different IDs across runs.
		/// This method serializes each event field-by-field with fixed-width
		/// types (matches Block F's actors subsystem feed) and resolves the
		/// StringIds to their content strings, producing a byte stream that's
		/// byte-identical across same-seed same-OS runs and identical
		/// modulo-endianness across same-arch OSes.
		void FeedToChecksum(const std::vector<Event>& events, const char* subsystemName) const;

		/// Set the current sim tick. Called once per tick by the runner.
		void SetCurrentTick(uint64_t tick) { m_CurrentTick.store(tick, std::memory_order_relaxed); }

		/// Get the current sim tick.
		uint64_t GetCurrentTick() const { return m_CurrentTick.load(std::memory_order_relaxed); }

		/// Convert a Layer enum to its string label.
		static const char* LayerName(Layer layer);

		/// Parse a layer name (case-insensitive). Returns Layer::Decision on unknown.
		static Layer LayerFromName(std::string_view name);

	private:
		std::atomic<uint64_t> m_CurrentTick{0};
		std::atomic<uint32_t> m_Sequence{0};

		mutable std::mutex                       m_StringMutex;
		std::vector<std::string>                 m_Strings;
		std::unordered_map<std::string, StringId> m_StringIndex;

		mutable std::mutex                       m_EventMutex;
		std::deque<Event>                        m_Events;

		// History per actor for the overlay (small fixed-size ring).
		static constexpr size_t kPerActorHistory = 32;
		mutable std::mutex                                m_HistoryMutex;
		std::unordered_map<int, std::deque<Event>>        m_History;
	};

} // namespace RTE
