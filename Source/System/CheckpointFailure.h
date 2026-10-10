#pragma once

#include <cstddef>
#include <new>

namespace RTE {
	/// Allocation failures used by checkpoint selftests, confined to the calling thread.
	class CheckpointFailure {
	public:
		enum class Point { None, NativePages, NativeRoots, NativeObjects, LuaPages, LuaSubmission, LuaAllocation, ArchiveSubmission, ArchiveValidation };
		class Scope {
		public:
			explicit Scope(Point point, size_t after = 0) : m_Point(s_Point), m_After(s_After) { s_Point = point; s_After = after; }
			~Scope() { s_Point = m_Point; s_After = m_After; }
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;
		private:
			Point m_Point;
			size_t m_After;
		};
		static bool Fails(Point point) noexcept {
			if (point != s_Point) return false;
			if (s_After) { --s_After; return false; }
			return true;
		}
		static Point Current() noexcept { return s_Point; }
		static void Check(Point point) { if (Fails(point)) throw std::bad_alloc(); }
	private:
		inline static thread_local Point s_Point = Point::None;
		inline static thread_local size_t s_After = 0;
	};
}
