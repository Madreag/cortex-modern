#pragma once

#include "CheckpointLuaHeap.h"
#include "SteadyClock.h"
#include "CheckpointBindingLease.h"
#include "CheckpointNativeStorage.h"

#include <algorithm>
#include <bit>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <utility>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <tuple>
#include <vector>

namespace RTE::CheckpointLua {
	// The saver prepares ranges and header storage while the live pages stay writable.
	class Boundary {
	public:
		struct Root {
			const void* source;
			size_t bytes;
			size_t alignment;
			template<class T> static Root Of(const T& source) { return {&source, sizeof(T), alignof(T)}; }
		};
		static std::unique_ptr<Boundary> Prepare(std::span<const lua_State* const> states, std::span<const Root> roots) {
			auto result = std::unique_ptr<Boundary>(new Boundary);
			result->m_Bindings = BindingLease::Prepare();
			size_t bytes = 0, alignment = 1;
			for (const Root& root: roots) {
				if (!root.source || !root.bytes || !std::has_single_bit(root.alignment))
					throw std::invalid_argument("invalid checkpoint boundary header");
				alignment = (std::max)(alignment, root.alignment);
				if (bytes > (std::numeric_limits<size_t>::max)() - root.alignment + 1)
					throw std::bad_array_new_length();
				bytes = (bytes + root.alignment - 1) & ~(root.alignment - 1);
				if (root.bytes > (std::numeric_limits<size_t>::max)() - bytes) throw std::bad_array_new_length();
				bytes += root.bytes;
			}
			CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			if (bytes > (std::numeric_limits<size_t>::max)() - alignment + 1) throw std::bad_array_new_length();
			result->m_HeaderBytes.reset(bytes ? new std::byte[bytes + alignment - 1] : nullptr);
			const auto start = (reinterpret_cast<uintptr_t>(result->m_HeaderBytes.get()) + alignment - 1) & ~(alignment - 1);
			result->m_Roots.reserve(roots.size());
			size_t offset = 0;
			for (const Root& root: roots) {
				offset = (offset + root.alignment - 1) & ~(root.alignment - 1);
				result->m_Roots.push_back({root.source, reinterpret_cast<const void*>(start + offset), root.bytes});
				offset += root.bytes;
			}
			result->m_Heaps.reserve(states.size());
			for (const lua_State* state: states) {
				const auto lease = HeapOwner::LeasePreparation(const_cast<lua_State*>(state));
				if (!lease) throw std::runtime_error("checkpoint Lua heap has no preparation owner");
				result->m_Heaps.push_back({lease, lease.Prepare()});
			}
			result->m_Native = CheckpointNativeStorage::PrepareInventory();
			return result;
		}
		/// Commits every copy and makes the pages a boundary writes resident, ahead of a capture; a boundary begun meanwhile
		/// ends it and commits the rest itself.
		void Warm() {
			for (const Heap& heap: m_Heaps) {
				heap.lease.FillHoles(*heap.prepared);
				heap.prepared->Warm();
			}
			try { m_Native->Warm(); } catch (const std::logic_error&) {}
		}
		/// Gives cold copies back, retaining only native copy runs the last cooling pass left Hot.
		void Cool() noexcept {
			for (const Heap& heap: m_Heaps) heap.prepared->Cool();
			m_Native->Cool();
		}
		~Boundary() { Cancel(); }
		Boundary(const Boundary&) = delete;
		Boundary& operator=(const Boundary&) = delete;

		bool Current() const noexcept {
			if (m_Status.load(std::memory_order_acquire) != Status::Prepared || !m_Native || !m_Native->Current() || !m_Bindings.Current()) return false;
			for (const Heap& heap: m_Heaps) if (!heap.lease.Current(*heap.prepared)) return false;
			return true;
		}
		/// Names each heap's distance in ticks to its state's next full collection (one: the next tick), so the boundary copies
		/// a heap about to be collected whole and a saver copies the rest in the order they collect.
		void CollectionOrder(std::function<uint64_t(size_t heap)> ticksUntil) { m_TicksUntilCollection = std::move(ticksUntil); }
		bool Arm(std::array<int64_t, 7>* parts = nullptr) {
			const auto started = RTE::SteadyClock::now();
			for (const Heap& heap: m_Heaps) heap.prepared->StopWarming();
			if (!Current()) { m_Refusal = StaleReason(); m_Stale = m_Status.load(std::memory_order_acquire) == Status::Prepared; return false; }
			Status expected = Status::Prepared;
			if (!m_Status.compare_exchange_strong(expected, Status::Arming, std::memory_order_acq_rel)) { m_Refusal = "the plan was already used"; return false; }
			try {
				// The saver's walk asks what memory was free or retained as of this fence, not as the game moves it on.
				m_Held = CheckpointHeldMemory::Open();
				std::vector<uint64_t> until(m_Heaps.size(), 0);
				if (m_TicksUntilCollection) for (size_t index = 0; index < m_Heaps.size(); ++index) until[index] = m_TicksUntilCollection(index);
				OrderDrains(until);
				std::vector<bool> learn;
				const std::vector<bool> byCopy = CopiedWhole(until, learn);
				// The Lua heaps are fenced on another thread while this one arms the native pages.
				static const bool s_SerialArm = std::getenv("CCCP_TEST_CHECKPOINT_SERIAL_ARM") != nullptr;
				const size_t threads = s_SerialArm ? 1 : std::min<size_t>(4, std::max<size_t>(1, m_Heaps.size()));
				auto heaps = std::async(s_SerialArm ? std::launch::deferred : std::launch::async, [this, &byCopy, &learn, threads] {
					const auto begun = RTE::SteadyClock::now();
					// This thread plans every heap and holds the plans' locks until all are fenced. The copies are cut into chunks
					// of a megabyte that every arming thread takes in turn, so one large heap does not set the boundary's length;
					// then the heaps are fenced side by side.
					std::vector<HeapOwner::ArmSession> sessions(m_Heaps.size());
					bool armed = true;
					for (size_t index = 0; armed && index < m_Heaps.size(); ++index)
						armed = m_Heaps[index].lease.BeginArm(*m_Heaps[index].prepared, byCopy[index], learn[index], sessions[index]);
					struct Chunk { uint32_t heap, first, count; };
					std::vector<Chunk> chunks;
					std::vector<size_t> copied(m_Heaps.size(), 0);
					const size_t chunkPages = std::max<size_t>(1, (size_t{1} << 20) / std::max<size_t>(1, PageWriteFence::SystemPageBytes()));
					for (size_t index = 0; armed && index < sessions.size(); ++index) {
						for (const auto& [first, count]: sessions[index].Spans()) {
							for (size_t at = 0; at < count; at += chunkPages)
								chunks.push_back({static_cast<uint32_t>(index), static_cast<uint32_t>(first + at), static_cast<uint32_t>(std::min<size_t>(chunkPages, count - at))});
							copied[index] += count;
						}
					}
					const auto planned = RTE::SteadyClock::now();
					std::atomic<size_t> next{0};
					const auto copySome = [&chunks, &next, &sessions] {
						for (size_t at; (at = next.fetch_add(1, std::memory_order_relaxed)) < chunks.size();) sessions[chunks[at].heap].CopyPages(chunks[at].first, chunks[at].count);
					};
					if (armed) {
						std::vector<std::future<void>> helpers;
						for (size_t part = 1; part < threads; ++part) helpers.push_back(std::async(std::launch::async, copySome));
						copySome();
						for (auto& helper: helpers) helper.get();
					}
					const auto copiedAt = RTE::SteadyClock::now();
					const int64_t copyUs = std::chrono::duration_cast<std::chrono::microseconds>(copiedAt - planned).count();
					if (armed) {
						AssignArmParts(copied, threads);
						const auto fenceSome = [this, &sessions, &copied, copyUs](size_t part) {
							bool fenced = true;
							for (size_t index = 0; fenced && index < sessions.size(); ++index)
								if (m_ArmParts[index] == part) fenced = sessions[index].Finish(copied[index], copyUs);
							return fenced;
						};
						std::vector<std::future<bool>> helpers;
						for (size_t part = 1; part < threads; ++part) helpers.push_back(std::async(std::launch::async, fenceSome, part));
						armed = fenceSome(0);
						for (auto& helper: helpers) armed = helper.get() && armed;
					}
					const auto finished = RTE::SteadyClock::now();
					const auto us = [](auto from, auto to) { return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count(); };
					m_ArmPhaseUs = {us(begun, planned), copyUs, us(copiedAt, finished)};
					return std::pair{armed, us(begun, finished)};
				});
				bool nativeArmed = false;
				std::exception_ptr failure;
				try { nativeArmed = m_Native->Arm(); } catch (...) { failure = std::current_exception(); }
				const auto nativeFenced = RTE::SteadyClock::now();
				const auto [heapsArmed, heapsUs] = heaps.get();
				const auto fenced = RTE::SteadyClock::now();
				if (failure) std::rethrow_exception(failure);
				if (!nativeArmed || !heapsArmed) {
					// The native inventory read current before it was fenced; one that grew meanwhile is a plan the world outgrew.
					m_Stale = !nativeArmed && !m_Native->Current();
					m_Refusal = !nativeArmed ? (m_Stale ? "the native inventory grew while it was fenced" : "the native pages could not be fenced") : "a Lua heap could not be fenced";
					Cancel();
					return false;
				}
				m_ArmSplitUs = {std::chrono::duration_cast<std::chrono::microseconds>(nativeFenced - started).count(), heapsUs};
				for (const auto& root: m_Roots) std::memcpy(const_cast<void*>(root.view), root.source, root.bytes);
				if (parts) {
					(*parts)[1] = std::chrono::duration_cast<std::chrono::microseconds>(fenced - started).count();
					(*parts)[2] = std::chrono::duration_cast<std::chrono::microseconds>(RTE::SteadyClock::now() - fenced).count();
				}
				m_Status.store(Status::Armed, std::memory_order_release);
				return true;
			} catch (...) { Cancel(); throw; }
		}
		void Drain() {
			if (m_Status.load(std::memory_order_acquire) != Status::Armed) throw std::logic_error("checkpoint boundary is not armed");
			try {
				// A busy heap the boundary left mostly fenced is copied at once, beside the game: the ticks after the boundary write
				// it all over, and as their own faults and as its collections that cost the game several times more. The heaps
				// drain before the native pages, which the game writes far less.
				constexpr size_t c_EagerFencedBytes = size_t{32} << 20;
				for (const Heap& heap: m_Heaps)
					if (heap.prepared->Image().Busy() && heap.prepared->FencedBytes() >= c_EagerFencedBytes) heap.prepared->CopyFencedNow();
				DrainHeaps();
				for (const auto& pages: m_Native->Pages()) pages->Drain();
				CheckpointFailure::Check(CheckpointFailure::Point::NativeRoots);
				m_ReadRoots = m_Roots;
				m_ReadRoots.reserve(m_Roots.size() + m_Heaps.size());
				for (const Heap& heap: m_Heaps) {
					const auto [source, bytes] = heap.prepared->Image().NativeBytes();
					m_ReadRoots.push_back({source, bytes.data(), bytes.size()});
				}
				m_Status.store(Status::Frozen, std::memory_order_release);
			} catch (...) { Cancel(); throw; }
		}
		void Cancel() noexcept {
			const Status previous = m_Status.exchange(Status::Cancelled, std::memory_order_acq_rel);
			if (previous == Status::Cancelled) return;
			if (m_Native) m_Native->Cancel();
			for (Heap& heap: m_Heaps) heap.prepared->Cancel();
		}
		CheckpointNativeStorage::ReadViews NativeViews() const {
			if (m_Status.load(std::memory_order_acquire) != Status::Frozen) throw std::logic_error("checkpoint boundary is not drained");
			return {true, m_Native->Pages(), m_ReadRoots};
		}
		Snapshot LuaHeap(size_t index) const {
			if (m_Status.load(std::memory_order_acquire) != Status::Frozen) throw std::logic_error("checkpoint boundary is not drained");
			return m_Heaps.at(index).prepared->Image();
		}
		size_t StateCount() const { return m_Heaps.size(); }
		/// The fence's own clock: the native ranges, then every Lua heap.
		std::pair<int64_t, int64_t> ArmSplitUs() const { return m_ArmSplitUs; }
		/// The arming thread that fenced a heap.
		size_t ArmPart(size_t heap) const { return heap < m_ArmParts.size() ? m_ArmParts[heap] : 0; }
		/// The Lua arm's plan, copy and fence, in microseconds.
		std::array<int64_t, 3> ArmPhaseUs() const { return m_ArmPhaseUs; }
		/// Whether the saver had warmed this plan when it armed, how long before the arm its warm was asked (-1: not asked), and
		/// how long the saver's last finished warm took.
		void NoteWarm(bool warmed, int64_t askedUsBefore, int64_t warmUs) noexcept { m_WarmAtArm = warmed; m_WarmAskedUsBefore = askedUsBefore; m_WarmUs = warmUs; }
		std::tuple<bool, int64_t, int64_t> WarmAtArm() const noexcept { return {m_WarmAtArm, m_WarmAskedUsBefore, m_WarmUs}; }
		const BindingLease& Bindings() const { return m_Bindings; }
		/// Why the last Arm refused, or null; and whether it refused because the world outgrew the plan, which a new plan cures.
		const char* Refusal() const noexcept { return m_Refusal; }
		bool RefusedStale() const noexcept { return m_Stale; }
		/// What the engine held free or retained at the fence, for the walk of the frozen script graphs.
		const CheckpointHeldMemory* HeldMemory() const noexcept { return m_Held.get(); }
		/// Ends the record once the script graphs are walked; nothing after them asks.
		void CloseHeldMemory() const noexcept { m_Held.reset(); }
		static std::string SelfTestMismatch() {
			CheckpointNativeStorage::AllocationScope allocations(true);
			auto state = HeapOwner::Create();
			auto other = HeapOwner::Create();
			lua_State* const lua = state->State();
			lua_State* const otherLua = other->State();
			struct Value : CheckpointNativeAllocated { uint64_t number = 7; };
			auto value = std::make_unique<Value>();
			const Value* const identity = value.get();
			struct Header { const Value* value; uint64_t tick; } header{identity, 1};
			const std::array<const lua_State*, 2> states{lua, otherLua};
			const std::array roots{Root::Of(header)};
			uint64_t* inlineValue = nullptr;
			ProtectedCall(lua, [&] {
				lua_pushinteger(lua, 11); lua_setglobal(lua, "boundaryValue");
				inlineValue = static_cast<uint64_t*>(lua_newuserdata(lua, sizeof(uint64_t)));
				*inlineValue = 29; lua_setglobal(lua, "boundaryNativeValue");
			});
			auto prepared = std::async(std::launch::async, [&] { auto boundary = Prepare(states, roots); boundary->Warm(); return boundary; }).get();
			header.tick = 2; value->number = 13;
			ProtectedCall(lua, [&] { lua_pushinteger(lua, 17); lua_setglobal(lua, "boundaryValue"); });
			if (!prepared->Arm()) return "prepared native and Lua boundary refused";
			*inlineValue = 31;
			header.tick = 3; value->number = 19;
			ProtectedCall(lua, [&] { lua_pushinteger(lua, 23); lua_setglobal(lua, "boundaryValue"); });
			value.reset();
			std::async(std::launch::async, [&] { prepared->Drain(); }).get();
			{
				CheckpointNativeStorage::ReadScope read(prepared->NativeViews());
				const auto* frozenHeader = CheckpointNativeStorage::Source(&header);
				if (frozenHeader->tick != 2 || frozenHeader->value != identity || CheckpointNativeStorage::Source(identity)->number != 13)
					return "native boundary lost its tick or object identity";
				const auto* frozenInline = CheckpointNativeStorage::Source(inlineValue);
				if (*frozenInline != 29 || CheckpointNativeStorage::Original(frozenInline) != inlineValue)
					return "native values in the Lua heap lost their frozen identity";
				const TValue number = prepared->LuaHeap(0).GlobalValue("boundaryValue");
				if (!tvisnumber(&number) || numberVnum(&number) != 17) return "Lua boundary differs from the native tick";
			}
			prepared.reset();
			auto stale = std::async(std::launch::async, [&] { return Prepare(states, roots); }).get();
			const size_t growth = stale->m_Heaps[1].prepared->Image().ByteCount() + Snapshot::c_PageBytes;
			const std::string text(growth, 'x');
			ProtectedCall(otherLua, [&] { lua_pushlstring(otherLua, text.data(), text.size()); lua_setglobal(otherLua, "growth"); });
			if (stale->Current() || stale->Arm()) return "grown Lua heap kept a stale boundary";
			stale->Cancel();
			bool refused = false;
			{
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
				try { Prepare(states, roots); } catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			}
			if (!refused) return "boundary header allocation failure was not refused";
			refused = false;
			{
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativePages);
				try { Prepare(states, roots); } catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			}
			if (!refused) return "native preparation failure left the Lua fences published";
			auto retry = std::async(std::launch::async, [&] { return Prepare(states, roots); }).get();
			header.tick = 4;
			if (!retry->Arm()) return "boundary refusal prevented a retry";
			header.tick = 5;
			std::async(std::launch::async, [&] { retry->Drain(); }).get();
			CheckpointNativeStorage::ReadScope read(retry->NativeViews());
			return CheckpointNativeStorage::Source(&header)->tick == 4 ? std::string() : "retried boundary header changed";
		}
	private:
		Boundary() = default;
		enum class Status { Prepared, Arming, Armed, Frozen, Cancelled };
		const char* StaleReason() const noexcept {
			if (m_Status.load(std::memory_order_acquire) != Status::Prepared) return "the plan was already used";
			if (!m_Native || !m_Native->Current()) return "the native inventory grew since the plan";
			if (!m_Bindings.Current()) return "the script bindings changed since the plan";
			return "a Lua heap grew or closed since the plan";
		}
		struct Heap { HeapOwner::PreparationLease lease; std::shared_ptr<HeapOwner::Prepared> prepared; };
		// A collection writes every page of its state's heap, so the heap collected in the next tick, which comes before any
		// idle time, and those collected soon after while they fit a couple of milliseconds of copying, are copied whole at
		// the boundary instead of fenced; a saver copies the rest in the order they collect. A busy heap (see
		// HeapOwner::NoteFullCollection) whose hot runs cover less than an eighth of it has not learned what the ticks after a
		// boundary write: it is copied whole and fenced to learn them, while all the whole copies fit c_BusyCopyBytes.
		std::vector<bool> CopiedWhole(const std::vector<uint64_t>& until, std::vector<bool>& learn) const {
			constexpr size_t c_WholeCopyBytes = size_t{16} << 20, c_BusyCopyBytes = size_t{96} << 20;
			std::vector<bool> whole(m_Heaps.size(), false);
			learn.assign(m_Heaps.size(), false);
			size_t bytes = 0;
			for (const size_t index: m_DrainOrder) {
				if (!until[index]) break;
				const size_t heapBytes = m_Heaps[index].prepared->Image().ByteCount();
				if (until[index] > 1 && bytes + heapBytes > c_WholeCopyBytes) break;
				whole[index] = true;
				bytes += heapBytes;
			}
			for (const size_t index: m_DrainOrder) {
				const auto image = m_Heaps[index].prepared->Image();
				if (whole[index] || !image.Busy() || m_Heaps[index].prepared->HotBytes() * 8 >= image.ByteCount() || bytes + image.ByteCount() > c_BusyCopyBytes) continue;
				whole[index] = true;
				learn[index] = true;
				bytes += image.ByteCount();
			}
			return whole;
		}
		// The heaps are fenced side by side, each by the arming thread with the least to fence so far, the largest first.
		void AssignArmParts(const std::vector<size_t>& copiedPages, size_t parts) {
			std::vector<size_t> cost(m_Heaps.size()), order(m_Heaps.size());
			const size_t pageBytes = PageWriteFence::SystemPageBytes();
			for (size_t index = 0; index < m_Heaps.size(); ++index) {
				const size_t bytes = m_Heaps[index].prepared->Image().ByteCount();
				cost[index] = bytes - std::min(bytes, copiedPages[index] * pageBytes);
			}
			std::iota(order.begin(), order.end(), size_t{0});
			std::stable_sort(order.begin(), order.end(), [&cost](size_t left, size_t right) { return cost[left] > cost[right]; });
			std::vector<size_t> load(parts, 0);
			m_ArmParts.assign(m_Heaps.size(), 0);
			for (const size_t index: order) {
				const size_t part = static_cast<size_t>(std::min_element(load.begin(), load.end()) - load.begin());
				m_ArmParts[index] = static_cast<uint8_t>(part);
				load[part] += cost[index];
			}
		}
		// Heaps whose state collects sooner copy first, then the larger ones, so the live writes that fault on a page not yet
		// copied stop soonest.
		void OrderDrains(const std::vector<uint64_t>& until) {
			std::vector<size_t> bytes(m_Heaps.size());
			for (size_t index = 0; index < m_Heaps.size(); ++index) bytes[index] = m_Heaps[index].prepared->Image().ByteCount();
			m_DrainOrder.resize(m_Heaps.size());
			std::iota(m_DrainOrder.begin(), m_DrainOrder.end(), size_t{0});
			const auto soon = [&until](size_t index) { return until[index] ? until[index] : (std::numeric_limits<uint64_t>::max)(); };
			std::stable_sort(m_DrainOrder.begin(), m_DrainOrder.end(), [&](size_t left, size_t right) {
				return soon(left) != soon(right) ? soon(left) < soon(right) : bytes[left] > bytes[right];
			});
		}
		// A saver copies the heaps side by side in the game's idle time, a few at once so the memory bandwidth goes to the
		// heaps that collect first.
		void DrainHeaps() {
			if (m_DrainOrder.size() != m_Heaps.size()) {
				m_DrainOrder.resize(m_Heaps.size());
				std::iota(m_DrainOrder.begin(), m_DrainOrder.end(), size_t{0});
			}
			if (!SaverThreads::Current() || m_Heaps.size() < 2) {
				for (const size_t index: m_DrainOrder) m_Heaps[index].prepared->Drain();
				return;
			}
			ParallelWork drains(SaverThreads::Pool(), m_DrainOrder.size(), [this](size_t index) { m_Heaps[m_DrainOrder[index]].prepared->Drain(); }, 2);
			drains.Finish();
		}
		std::function<uint64_t(size_t)> m_TicksUntilCollection;
		std::vector<size_t> m_DrainOrder;
		std::vector<uint8_t> m_ArmParts;
		std::array<int64_t, 3> m_ArmPhaseUs{};
		bool m_WarmAtArm = false;
		int64_t m_WarmAskedUsBefore = -1, m_WarmUs = 0;
		std::atomic<Status> m_Status{Status::Prepared};
		const char* m_Refusal = nullptr;
		bool m_Stale = false;
		std::unique_ptr<CheckpointNativeStorage::Prepared> m_Native;
		mutable std::shared_ptr<CheckpointHeldMemory> m_Held;
		BindingLease m_Bindings;
		std::vector<Heap> m_Heaps;
		std::unique_ptr<std::byte[]> m_HeaderBytes;
		std::vector<CheckpointNativeStorage::RootRange> m_Roots;
		std::vector<CheckpointNativeStorage::RootRange> m_ReadRoots;
		std::pair<int64_t, int64_t> m_ArmSplitUs{};
	};
}
