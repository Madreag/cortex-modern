#pragma once

#include <cstddef>
#include <atomic>
#include <memory>
#include <new>

namespace RTE {
	/// Allocation failures stay with the capture when its worker changes.
	class CheckpointFailure {
	public:
		enum class Point { None, NativePages, NativeRoots, NativeObjects, LuaPages, LuaSubmission, LuaAllocation, ArchiveSubmission, ArchiveValidation, ParallelSubmission, CopyStartup, CopyWatch };
		struct State {
			State(Point point, size_t after) : point(point), after(after) {}
			const Point point;
			std::atomic<size_t> after;
			std::atomic<bool> triggered{false};
		};
		using Token = std::shared_ptr<State>;
		class Scope {
		public:
			explicit Scope(Point point, size_t after = 0) : m_Previous(s_State), m_State(point == Point::None ? nullptr : std::make_shared<State>(point, after)) { s_State = m_State; }
			~Scope() { s_State = std::move(m_Previous); }
			bool Triggered() const noexcept { return m_State && m_State->triggered.load(std::memory_order_acquire); }
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;
		private:
			Token m_Previous, m_State;
		};
		class ForwardScope {
		public:
			explicit ForwardScope(const Token& state) noexcept : m_Previous(std::move(s_State)) { s_State = state; }
			~ForwardScope() { s_State = std::move(m_Previous); }
			ForwardScope(const ForwardScope&) = delete;
			ForwardScope& operator=(const ForwardScope&) = delete;
		private:
			Token m_Previous;
		};
		static bool Fails(Point point) noexcept {
			if (!s_State || point != s_State->point) return false;
			size_t remaining = s_State->after.load(std::memory_order_relaxed);
			while (remaining) {
				if (s_State->after.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel)) return false;
			}
			s_State->triggered.store(true, std::memory_order_release);
			return true;
		}
		static Point Current() noexcept { return s_State ? s_State->point : Point::None; }
		static Token Capture() noexcept { return s_State; }
		static void Check(Point point) { if (Fails(point)) throw std::bad_alloc(); }
	private:
		inline static thread_local Token s_State;
	};
}
