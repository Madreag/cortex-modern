#include "CheckpointPagePool.h"
#include "SteadyClock.h"

#include "PageWriteFence.h"
#include "CheckpointPageCopy.h"
#include "CheckpointFailure.h"
#include "CheckpointNativeStorage.h"
#include "CheckpointNativeSnapshot.h"
#include "CheckpointString.h"
#include "CheckpointFrozenContainers.h"
#include "CheckpointArchive.h"
#include "Singleton.h"
#include "ScenarioRunner.h"
#include "Vector.h"
#include "ThreadMan.h"
#include "System.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <future>
#include <iterator>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#ifndef _MSC_VER
#include <cxxabi.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#endif

using namespace RTE;

namespace {
	int64_t Since(RTE::SteadyClock::time_point start) {
		return std::chrono::duration_cast<std::chrono::microseconds>(RTE::SteadyClock::now() - start).count();
	}
	struct Pages {
		unsigned char* data = nullptr;
		size_t bytes;
		bool betweenTicks = false; //!< A capture's copy goes back between the game's ticks.
		/// @param reserved Address space only: Commit makes each range usable as it is needed.
		explicit Pages(size_t bytes, bool betweenTicks = false, bool reserved = false) : bytes(bytes), betweenTicks(betweenTicks) {
			CheckpointFailure::Check(CheckpointFailure::Point::NativePages);
#ifdef _WIN32
			data = static_cast<unsigned char*>(reserved ? VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS) : VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
			void* memory = mmap(nullptr, bytes, reserved ? PROT_NONE : PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (memory != MAP_FAILED) data = static_cast<unsigned char*>(memory);
#endif
			if (!data) throw std::bad_alloc();
		}
		/// False when the process may take no more memory.
		bool Commit(size_t offset, size_t size) noexcept {
#ifdef _WIN32
			return VirtualAlloc(data + offset, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
			return mprotect(data + offset, size, PROT_READ | PROT_WRITE) == 0;
#endif
		}
		~Pages() {
			if (betweenTicks) ReleaseBetweenTicks(data, bytes);
			else Release(data, bytes);
		}
		static void Release(unsigned char* data, size_t bytes) noexcept {
#ifdef _WIN32
			VirtualFree(data, 0, MEM_RELEASE);
#else
			munmap(data, bytes);
#endif
		}
		// One large release holds the process's address space while the game faults on its own pages, so a saver gives
		// the copy back a piece at a time, and a game thread leaves it to the savers.
		static void ReleaseBetweenTicks(unsigned char* data, size_t bytes) noexcept {
			if (!SaverThreads::Current()) {
				if (SaverThreads::Gated()) try {
					SaverThreads::Pool().push_task([data, bytes] { ReleaseBetweenTicks(data, bytes); });
					return;
				} catch (...) {}
				Release(data, bytes);
				return;
			}
			constexpr size_t piece = size_t{1} << 20;
			for (size_t offset = 0; offset + piece < bytes; offset += piece) {
				try { SaverThreads::YieldToGame(); } catch (...) {}
#ifdef _WIN32
				VirtualFree(data + offset, piece, MEM_DECOMMIT);
#else
				madvise(data + offset, piece, MADV_DONTNEED);
#endif
			}
			Release(data, bytes);
		}
	};
	struct Locked {
		std::atomic_flag& flag;
		explicit Locked(std::atomic_flag& flag) : flag(flag) { while (flag.test_and_set(std::memory_order_acquire)) {} }
		~Locked() { flag.clear(std::memory_order_release); }
	};
	// The private memory the process has committed, as a commit limit counts it; -1 where the platform charges only touched pages.
	int64_t CommittedPrivateBytes() {
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS_EX counters{};
		if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return -1;
		return static_cast<int64_t>(counters.PrivateUsage);
#elif defined(__linux__)
		FILE* status = std::fopen("/proc/self/status", "r");
		if (!status) return -1;
		char line[256];
		long long kilobytes = -1;
		while (std::fgets(line, sizeof(line), status)) if (std::sscanf(line, "VmData: %lld kB", &kilobytes) == 1) break;
		std::fclose(status);
		return kilobytes < 0 ? -1 : kilobytes * 1024;
#else
		return -1;
#endif
	}
}

struct CheckpointPagePool::ArmGate {
	enum class State { Prepared, Arming, Armed, Cancelled };
	std::atomic<State> state{State::Prepared};
	std::thread::id simThread;
	size_t copiedPages = 0, fencedPages = 0;
	void Publish(State next) { state.store(next, std::memory_order_release); state.notify_all(); }
};

struct CheckpointPagePool::Copy {
	Pages pages;
	std::vector<std::atomic<unsigned char>> saved;
	std::vector<unsigned char> hot;
	// An inventory's copy is address space until a page is saved into it: a boundary keeps only the pages written while it is
	// read and the pages its reader copies, never a second copy of the whole inventory. Under the block's lock.
	std::vector<unsigned char> committed;
	size_t chunkBytes = 0;
	std::atomic<uint64_t> committedBytes{0};
	std::atomic<bool> exhausted{false}; //!< A page this copy had to keep found no memory, so its frozen view cannot be read.
	std::thread::id simThread;
	std::atomic<bool> armed{false};
	std::atomic<int64_t> workerUs{0}, simFaultNs{0}, otherFaultNs{0};
	std::atomic<uint64_t> simFaults{0}, otherFaults{0};
	bool completed = false;
	bool abandoned = false;
	const std::shared_ptr<ArmGate> gate;
	Copy(size_t bytes, size_t pageBytes, std::shared_ptr<ArmGate> gate = {}) : pages(bytes, true, gate != nullptr), saved(bytes / pageBytes), hot(bytes / pageBytes, 0), gate(std::move(gate)) {
		for (auto& page: saved) page.store(0, std::memory_order_relaxed);
		if (this->gate) {
			chunkBytes = std::max<size_t>(size_t{64} << 10, pageBytes);
			committed.assign((bytes + chunkBytes - 1) / chunkBytes, 0);
		} else {
			committedBytes.store(bytes, std::memory_order_relaxed);
		}
	}
	/// Makes the copy's pages [first, first + count) usable; the caller holds the block's lock.
	bool Commit(size_t first, size_t count, size_t pageBytes) noexcept {
		if (committed.empty() || !count) return true;
		const size_t last = ((first + count) * pageBytes - 1) / chunkBytes;
		for (size_t chunk = first * pageBytes / chunkBytes; chunk <= last; ++chunk) {
			if (committed[chunk]) continue;
			const size_t offset = chunk * chunkBytes, size = std::min(chunkBytes, pages.bytes - offset);
			if (!pages.Commit(offset, size)) {
				exhausted.store(true, std::memory_order_release);
				return false;
			}
			committed[chunk] = 1;
			committedBytes.fetch_add(size, std::memory_order_relaxed);
		}
		return true;
	}
	bool Arming() const noexcept { return gate && gate->state.load(std::memory_order_acquire) == ArmGate::State::Arming; }
	bool Armed() const noexcept { return gate ? gate->state.load(std::memory_order_acquire) == ArmGate::State::Armed : armed.load(std::memory_order_acquire); }
	std::thread::id SimulationThread() const noexcept { return gate ? gate->simThread : simThread; }
};

namespace {
	std::atomic<uint64_t> s_TallyFaults{0}, s_TallyFaultNs{0}, s_TallySettled{0}, s_TallyCooled{0};
}

bool CheckpointPagePool::TallyingFaults() {
	static const bool tallying = std::getenv("CCCP_TEST_FAULT_TICKS") != nullptr;
	return tallying;
}

CheckpointPagePool::FaultTally CheckpointPagePool::TakeFaultTally() {
	return {s_TallyFaults.exchange(0), s_TallyFaultNs.exchange(0), s_TallySettled.exchange(0), s_TallyCooled.exchange(0)};
}

struct CheckpointPagePool::Block {
	// Between autosaves an inventory page is open and not yet seen, open and written since, or fenced and unchanged.
	enum class PageState : unsigned char { Open, Hot, Fenced };
	Pages live;
	size_t slotBytes;
	size_t pageBytes = PageWriteFence::SystemPageBytes();
	std::atomic_flag lock = ATOMIC_FLAG_INIT;
	std::vector<std::shared_ptr<Copy>> copies;
	std::vector<PageState> states;
	std::vector<uint64_t> digests; // Each written page's bytes at the last cooling pass; only the settler reads them.
	explicit Block(size_t bytes, size_t slotBytes = 0) : live(bytes), slotBytes(slotBytes ? slotBytes : bytes), states(bytes / pageBytes, PageState::Open) {}
	~Block() { PageWriteFence::UnwatchCopies(this); }
	bool Watch() {
		const auto keepWritable = [](void*, uintptr_t, size_t) noexcept { return true; };
		return PageWriteFence::WatchCopies(this, {live.data, live.bytes}, OnWrite, this, keepWritable, nullptr, nullptr, OnFenceLifted);
	}
	void Prepare(const std::shared_ptr<Copy>& copy) {
		if (!Watch()) throw std::runtime_error("could not prepare native checkpoint page watches");
		Locked guard(lock);
		std::erase_if(copies, [](const auto& value) { return value->completed; });
		copies.push_back(copy);
	}
	void Arm(const std::shared_ptr<Copy>& copy) {
		Locked guard(lock);
		if (copy->abandoned) throw std::logic_error("native checkpoint preparation was rolled back");
		if (copy->armed.load(std::memory_order_relaxed)) return;
		copy->simThread = std::this_thread::get_id();
		if (!PageWriteFence::ProtectCopyPages(reinterpret_cast<uintptr_t>(live.data), live.bytes))
			throw std::runtime_error("could not fence native checkpoint pages");
		std::fill(states.begin(), states.end(), PageState::Fenced);
		copy->armed.store(true, std::memory_order_release);
	}
	// An inventory boundary copies the pages written since the last one and fences the pages it has not seen; fenced pages stay fenced.
	std::pair<size_t, size_t> ArmInventory(Copy& copy) {
		size_t copied = 0, fenced = 0;
		Locked guard(lock);
		if (copy.abandoned) throw std::logic_error("native checkpoint preparation was rolled back");
		for (size_t page = 0; page < states.size();) {
			if (states[page] == PageState::Hot) {
				if (!copy.saved[page].load(std::memory_order_relaxed)) {
					if (!copy.Commit(page, 1, pageBytes)) throw std::bad_alloc();
					CopyCheckpointPages(copy.pages.data + page * pageBytes, live.data + page * pageBytes, pageBytes);
					copy.saved[page].store(1, std::memory_order_release);
					++copied;
				}
				copy.hot[page] = 1;
				++page;
			} else if (states[page] == PageState::Open) {
				size_t end = page + 1;
				while (end < states.size() && states[end] == PageState::Open) ++end;
				if (!PageWriteFence::ProtectCopyPages(reinterpret_cast<uintptr_t>(live.data + page * pageBytes), (end - page) * pageBytes))
					throw std::runtime_error("could not fence native checkpoint pages");
				std::fill(states.begin() + page, states.begin() + end, PageState::Fenced);
				fenced += end - page;
				page = end;
			} else {
				++page;
			}
		}
		return {copied, fenced};
	}
	bool SavePages(size_t first, size_t count, bool fault) noexcept {
		const auto started = fault ? RTE::SteadyClock::now() : RTE::SteadyClock::time_point{};
		Locked guard(lock);
		// A write fault saves and opens the aligned run around it, between saves too: taking a fault costs several times
		// what copying a page does, and the game's writes come in runs.
		constexpr size_t faultRun = 16;
		const size_t faulted = fault && count == 1 ? first : (std::numeric_limits<size_t>::max)();
		if (fault && count == 1) {
			first -= first % faultRun;
			count = std::min(faultRun, states.size() - first);
		}
		// Every capture whose boundary has begun keeps the page as that boundary saw it.
		for (const auto& copy: copies) {
			if (copy->completed || copy->abandoned || copy->exhausted.load(std::memory_order_relaxed) || !(copy->Armed() || copy->Arming())) continue;
			for (size_t page = first; page < first + count; ++page) {
				if (copy->saved[page].load(std::memory_order_relaxed)) continue;
				// Without memory for the page its reader is refused; the game's write still goes ahead.
				if (!copy->Commit(page, 1, pageBytes)) break;
				const size_t offset = page * pageBytes;
				CopyCheckpointPages(copy->pages.data + offset, live.data + offset, pageBytes);
				copy->saved[page].store(1, std::memory_order_release);
			}
		}
		if (!fault) return true;
		// A written page stays open, and the next boundary copies it instead of fencing it again. Opening a page that is
		// open already makes the game's next write to it take a fault, so the faulting page and the fenced ones open.
		bool opened = true;
		for (size_t page = first; opened && page < first + count;) {
			const auto fenced = [&](size_t at) { return states[at] == PageState::Fenced || at == faulted; };
			if (!fenced(page)) { ++page; continue; }
			const size_t begin = page;
			while (page < first + count && fenced(page)) ++page;
			opened = PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(live.data + begin * pageBytes), (page - begin) * pageBytes);
			if (opened) std::fill(states.begin() + begin, states.begin() + page, PageState::Hot);
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(RTE::SteadyClock::now() - started).count();
		if (TallyingFaults()) { s_TallyFaults.fetch_add(1, std::memory_order_relaxed); s_TallyFaultNs.fetch_add(elapsed, std::memory_order_relaxed); }
		for (const auto& copy: copies) if (copy->Armed() && !copy->completed) {
			const bool simulation = std::this_thread::get_id() == copy->SimulationThread();
			(simulation ? copy->simFaults : copy->otherFaults).fetch_add(1, std::memory_order_relaxed);
			(simulation ? copy->simFaultNs : copy->otherFaultNs).fetch_add(elapsed, std::memory_order_relaxed);
		}
		return opened;
	}
	bool SavePage(size_t page, bool fault) noexcept { return SavePages(page, 1, fault); }
	static bool OnWrite(void* context, uintptr_t address) noexcept {
		auto& block = *static_cast<Block*>(context);
		return block.SavePage((address - reinterpret_cast<uintptr_t>(block.live.data)) / block.pageBytes, true);
	}
	// A preview's page fence hands its pages back: those this block keeps fenced stay read-only, the rest open.
	static bool OnFenceLifted(void* context, uintptr_t address, size_t bytes) noexcept {
		auto& block = *static_cast<Block*>(context);
		Locked guard(block.lock);
		const size_t first = (address - reinterpret_cast<uintptr_t>(block.live.data)) / block.pageBytes, end = first + bytes / block.pageBytes;
		bool opened = true;
		for (size_t page = first; page < end;) {
			if (block.states[page] == PageState::Fenced) { ++page; continue; }
			size_t run = page + 1;
			while (run < end && block.states[run] != PageState::Fenced) ++run;
			opened = PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(block.live.data + page * block.pageBytes), (run - page) * block.pageBytes) && opened;
			page = run;
		}
		return opened;
	}
	bool Active() const { return std::any_of(copies.begin(), copies.end(), [](const auto& value) { return value->Arming() || (value->Armed() && !value->completed); }); }
	void OpenAll() {
		if (PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(live.data), live.bytes)) std::fill(states.begin(), states.end(), PageState::Open);
	}
	void Complete(const std::shared_ptr<Copy>& copy) {
		Locked guard(lock);
		copy->completed = true;
		if (copy->gate) return;
		if (!Active() && !PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(live.data), live.bytes))
			throw std::runtime_error("could not open copied native checkpoint pages");
		if (!Active()) std::fill(states.begin(), states.end(), PageState::Open);
	}
	// Pages a boundary copied as written that have not changed since are fenced again, so the next boundary skips them.
	void Demote(const Copy& copy) noexcept {
		for (size_t page = 0; page < states.size();) {
			const auto still = [&](size_t index) {
				const size_t offset = index * pageBytes;
				return copy.hot[index] && states[index] == PageState::Hot && copy.saved[index].load(std::memory_order_acquire) &&
				    std::memcmp(copy.pages.data + offset, live.data + offset, pageBytes) == 0;
			};
			if (!still(page)) { ++page; continue; }
			size_t end = page + 1;
			while (end < states.size() && still(end)) ++end;
			if (PageWriteFence::ProtectCopyPages(reinterpret_cast<uintptr_t>(live.data + page * pageBytes), (end - page) * pageBytes))
				std::fill(states.begin() + page, states.begin() + end, PageState::Fenced);
			page = end;
		}
	}
	void Abandon(const std::shared_ptr<Copy>& copy) noexcept {
		Locked guard(lock);
		std::erase(copies, copy);
		copy->abandoned = true;
		copy->armed.store(false, std::memory_order_release);
		if (copy->gate) { Demote(*copy); return; }
		if (!Active()) OpenAll();
	}
	// Fences up to the given pages that no boundary has seen yet, so the first boundary of a world has little to fence.
	size_t Settle(size_t budget) {
		Locked guard(lock);
		size_t fenced = 0;
		for (size_t page = 0; page < states.size() && fenced < budget;) {
			if (states[page] != PageState::Open) { ++page; continue; }
			size_t end = page + 1;
			while (end < states.size() && states[end] == PageState::Open && fenced + (end - page) < budget) ++end;
			if (!PageWriteFence::ProtectCopyPages(reinterpret_cast<uintptr_t>(live.data + page * pageBytes), (end - page) * pageBytes)) return fenced;
			std::fill(states.begin() + page, states.begin() + end, PageState::Fenced);
			fenced += end - page;
			page = end;
		}
		return fenced;
	}
	// Fences again the written pages whose bytes did not change since the last pass, so a boundary copies only the pages
	// still being written; a page fenced too early costs one more write fault.
	size_t Cool() {
		constexpr size_t chunk = 1024;
		if (digests.size() != states.size()) digests.assign(states.size(), 0);
		std::array<PageState, chunk> seen;
		std::vector<unsigned char> still(chunk);
		size_t cooled = 0;
		for (size_t first = 0; first < states.size(); first += chunk) {
			SaverThreads::YieldToGame();
			const size_t count = std::min(chunk, states.size() - first);
			{
				Locked guard(lock);
				std::copy_n(states.begin() + first, count, seen.begin());
			}
			bool any = false;
			for (size_t index = 0; index < count; ++index) {
				still[index] = 0;
				if (seen[index] != PageState::Hot) continue;
				const uint64_t digest = PageDigest(live.data + (first + index) * pageBytes, pageBytes);
				still[index] = digest == digests[first + index];
				digests[first + index] = digest;
				any |= still[index] != 0;
			}
			if (!any) continue;
			Locked guard(lock);
			for (size_t index = 0; index < count;) {
				const auto unchanged = [&](size_t at) { return still[at] && states[first + at] == PageState::Hot; };
				if (!unchanged(index)) { ++index; continue; }
				size_t end = index + 1;
				while (end < count && unchanged(end)) ++end;
				if (PageWriteFence::ProtectCopyPages(reinterpret_cast<uintptr_t>(live.data + (first + index) * pageBytes), (end - index) * pageBytes)) {
					std::fill(states.begin() + first + index, states.begin() + first + end, PageState::Fenced);
					cooled += end - index;
				}
				index = end;
			}
		}
		return cooled;
	}
	static uint64_t PageDigest(const unsigned char* page, size_t bytes) noexcept {
		uint64_t lanes[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
		for (size_t offset = 0; offset + 32 <= bytes; offset += 32) {
			for (size_t lane = 0; lane < 4; ++lane) {
				uint64_t word;
				std::memcpy(&word, page + offset + lane * 8, sizeof(word));
				lanes[lane] = (lanes[lane] ^ word) * 0x100000001B3ull;
			}
		}
		return lanes[0] ^ (lanes[1] * 3) ^ (lanes[2] * 5) ^ (lanes[3] * 7);
	}
	size_t OpenPages() {
		Locked guard(lock);
		return static_cast<size_t>(std::count(states.begin(), states.end(), PageState::Open));
	}
	bool Contains(const void* source, size_t bytes) const {
		const uintptr_t from = reinterpret_cast<uintptr_t>(source), base = reinterpret_cast<uintptr_t>(live.data);
		return from >= base && from - base <= live.bytes && bytes <= live.bytes - (from - base);
	}
};

struct CheckpointPagePool::Registry {
	std::mutex mutex;
	std::map<uintptr_t, std::weak_ptr<Block>> blocks;
	std::atomic<uint64_t> generation{0};
	static inline std::atomic<Registry*> current{nullptr};
	static Registry& Get() {
		static auto* registry = [] { auto* value = new Registry; current.store(value, std::memory_order_release); return value; }();
		return *registry;
	}
};

void CheckpointPagePool::Register(const std::shared_ptr<Block>& block) {
	auto& registry = Registry::Get();
	std::lock_guard lock(registry.mutex);
	const auto address = reinterpret_cast<uintptr_t>(block->live.data);
	if (const auto found = registry.blocks.find(address); found != registry.blocks.end() && found->second.lock() == block) return;
	registry.blocks.insert_or_assign(address, block);
	registry.generation.fetch_add(1, std::memory_order_release);
}

void CheckpointPagePool::Unregister(const std::shared_ptr<Block>& block) noexcept {
	auto* registry = Registry::current.load(std::memory_order_acquire);
	if (!registry) return;
	std::lock_guard lock(registry->mutex);
	const auto found = registry->blocks.find(reinterpret_cast<uintptr_t>(block->live.data));
	if (found == registry->blocks.end() || found->second.lock() != block) return;
	registry->blocks.erase(found);
	registry->generation.fetch_add(1, std::memory_order_release);
}

void CheckpointPagePool::IncludeInInventory() {
	if (m_InInventory) return;
	for (const auto& block: m_Blocks) Register(block);
	m_InInventory = true;
}

uint64_t CheckpointPagePool::InventoryGeneration() noexcept {
	const auto* registry = Registry::current.load(std::memory_order_acquire);
	return registry ? registry->generation.load(std::memory_order_acquire) : 0;
}

std::vector<std::shared_ptr<CheckpointPagePool::Block>> CheckpointPagePool::LiveBlocks() {
	std::vector<std::shared_ptr<Block>> blocks;
	auto& registry = Registry::Get();
	std::lock_guard lock(registry.mutex);
	blocks.reserve(registry.blocks.size());
	for (const auto& [address, weak]: registry.blocks) if (auto block = weak.lock()) blocks.push_back(std::move(block));
	return blocks;
}

size_t CheckpointPagePool::CoolInventory() {
	// One pass at a time: the digests are the pass's own.
	static std::mutex passes;
	std::lock_guard pass(passes);
	size_t cooled = 0;
	for (const auto& block: LiveBlocks()) {
		SaverThreads::YieldToGame();
		if (block->Watch()) cooled += block->Cool();
	}
	if (TallyingFaults()) s_TallyCooled.fetch_add(cooled, std::memory_order_relaxed);
	return cooled;
}

size_t CheckpointPagePool::SettleInventory(size_t budget) {
	const auto blocks = LiveBlocks();
	size_t left = 0;
	const size_t asked = budget;
	for (const auto& block: blocks) {
		SaverThreads::YieldToGame();
		if (budget && block->Watch()) budget -= block->Settle(budget);
		left += block->OpenPages();
	}
	if (TallyingFaults()) s_TallySettled.fetch_add(asked - budget, std::memory_order_relaxed);
	return left;
}

std::shared_ptr<const CheckpointPagePool::Snapshot> CheckpointPagePool::PrepareInventory(uint64_t& generation) {
	auto snapshot = std::make_shared<Snapshot>();
	snapshot->m_ArmGate = std::make_shared<ArmGate>();
	{
		auto& registry = Registry::Get();
		std::lock_guard lock(registry.mutex);
		snapshot->m_Parts.reserve(registry.blocks.size());
		for (auto it = registry.blocks.begin(); it != registry.blocks.end();) {
			if (auto block = it->second.lock()) { snapshot->m_Parts.push_back({std::move(block), {}}); ++it; }
			else it = registry.blocks.erase(it);
		}
		generation = registry.generation.load(std::memory_order_relaxed);
	}
	// Shadow storage and page watches are built after releasing the live allocation inventory.
	for (auto& part: snapshot->m_Parts) {
		part.copy = std::make_shared<Copy>(part.block->live.bytes, part.block->pageBytes, snapshot->m_ArmGate);
		part.block->Prepare(part.copy);
	}
	snapshot->Index();
	snapshot->m_Ranges.reserve(snapshot->m_Parts.size());
	for (const auto& part: snapshot->m_Parts) {
		const uintptr_t first = reinterpret_cast<uintptr_t>(part.block->live.data);
#ifndef _WIN32
		if (!snapshot->m_Ranges.empty() && snapshot->m_Ranges.back().first + snapshot->m_Ranges.back().bytes == first)
			snapshot->m_Ranges.back().bytes += part.block->live.bytes;
		else
#endif
		// VirtualProtect cannot span independently reserved Windows allocations.
		snapshot->m_Ranges.push_back({first, part.block->live.bytes});
	}
	return snapshot;
}

std::pair<void*, size_t> CheckpointPagePool::Grow(size_t slotBytes, size_t minimumSlots, std::vector<void*>& free, size_t blockBytes) {
	const size_t pageBytes = PageWriteFence::SystemPageBytes();
	if (!slotBytes || !pageBytes || minimumSlots > (std::numeric_limits<size_t>::max() - pageBytes) / slotBytes)
		throw std::length_error("native checkpoint pool is too large");
	const size_t slots = std::max(minimumSlots, blockBytes / slotBytes);
	if (slots > std::numeric_limits<size_t>::max() - m_Slots) throw std::bad_alloc();
	const size_t bytes = (slots * slotBytes + pageBytes - 1) / pageBytes * pageBytes;
	auto block = std::make_shared<Block>(bytes, slotBytes);
	// Room for every slot keeps a returned slot allocation-free; it grows by half again so a class's growth stays linear.
	if (free.capacity() < m_Slots + slots) free.reserve(std::max(m_Slots + slots, free.capacity() + free.capacity() / 2));
	if (m_InInventory) Register(block);
	m_Blocks.push_back(block);
	for (size_t slot = 0; slot < slots; ++slot) free.push_back(block->live.data + slot * slotBytes);
	m_Slots += slots;
	return {block->live.data, block->live.bytes};
}

bool CheckpointPagePool::Contains(const void* address) const {
	return std::any_of(m_Blocks.begin(), m_Blocks.end(), [&](const auto& block) { return block->Contains(address, 1); });
}

CheckpointPagePool::Allocation::Allocation(size_t bytes, bool inventory) {
	const size_t page = PageWriteFence::SystemPageBytes();
	if (!page || bytes > std::numeric_limits<size_t>::max() - page) throw std::bad_alloc();
	m_Block = std::make_shared<Block>(std::max(page, (bytes + page - 1) / page * page));
	if (inventory) Register(m_Block);
}
void* CheckpointPagePool::Allocation::Data() const { return m_Block->live.data; }
size_t CheckpointPagePool::Allocation::Bytes() const { return m_Block->live.bytes; }
bool CheckpointPagePool::Allocation::Contains(const void* source, size_t bytes) const { return m_Block->Contains(source, bytes); }
void CheckpointPagePool::Allocation::IncludeInInventory() const { Register(m_Block); }
void CheckpointPagePool::Allocation::ExcludeFromInventory() const noexcept { Unregister(m_Block); }
std::shared_ptr<const CheckpointPagePool::Snapshot> CheckpointPagePool::Allocation::Freeze(bool arm) const {
	auto snapshot = std::make_shared<Snapshot>();
	snapshot->m_Parts.reserve(1);
	auto copy = std::make_shared<Copy>(m_Block->live.bytes, m_Block->pageBytes);
	snapshot->m_Parts.push_back({m_Block, std::move(copy)});
	m_Block->Prepare(snapshot->m_Parts.back().copy);
	snapshot->Index();
	if (arm) snapshot->Arm();
	return snapshot;
}

std::span<const std::byte> CheckpointPagePool::Snapshot::ReadBytes(const void* source, size_t bytes) const {
	const uintptr_t address = reinterpret_cast<uintptr_t>(source);
	const auto end = std::upper_bound(m_Parts.begin(), m_Parts.end(), address, [](uintptr_t address, const Part& part) {
		return address < reinterpret_cast<uintptr_t>(part.block->live.data);
	});
	if (end == m_Parts.begin() || !std::prev(end)->block->Contains(source, bytes))
		throw std::runtime_error("native checkpoint span lies outside its frozen allocation");
	const auto& part = *std::prev(end);
	if (!part.copy->Armed()) throw std::logic_error("native checkpoint read before its boundary");
	const size_t offset = address - reinterpret_cast<uintptr_t>(part.block->live.data);
	if (bytes) {
		const size_t first = offset / part.block->pageBytes, last = (offset + bytes - 1) / part.block->pageBytes;
		for (size_t page = first; page <= last; ++page) {
			if (!part.copy->saved[page].load(std::memory_order_acquire) && !part.block->SavePage(page, false))
				throw std::runtime_error("could not read a frozen native page");
		}
	}
	if (part.copy->exhausted.load(std::memory_order_acquire)) throw std::bad_alloc();
	return {reinterpret_cast<const std::byte*>(part.copy->pages.data + offset), bytes};
}

std::shared_ptr<const CheckpointPagePool::Snapshot> CheckpointPagePool::Freeze(bool arm) const {
	auto snapshot = std::make_shared<Snapshot>();
	snapshot->m_Parts.reserve(m_Blocks.size());
	for (const auto& block: m_Blocks) {
		auto copy = std::make_shared<Copy>(block->live.bytes, block->pageBytes);
		snapshot->m_Parts.push_back({block, std::move(copy)});
		block->Prepare(snapshot->m_Parts.back().copy);
	}
	snapshot->Index();
	if (arm) snapshot->Arm();
	return snapshot;
}

void CheckpointPagePool::Snapshot::Index() {
	std::sort(m_Parts.begin(), m_Parts.end(), [](const auto& a, const auto& b) {
		return reinterpret_cast<uintptr_t>(a.block->live.data) < reinterpret_cast<uintptr_t>(b.block->live.data);
	});
	m_Views.resize(m_Parts.size());
	for (size_t index = 0; index < m_Views.size(); ++index) m_Views[index] = index;
	std::sort(m_Views.begin(), m_Views.end(), [this](size_t a, size_t b) {
		return reinterpret_cast<uintptr_t>(m_Parts[a].copy->pages.data) < reinterpret_cast<uintptr_t>(m_Parts[b].copy->pages.data);
	});
}

const void* CheckpointPagePool::Snapshot::ReadAllocation(const void* source) const {
	const uintptr_t address = reinterpret_cast<uintptr_t>(source);
	const auto end = std::upper_bound(m_Parts.begin(), m_Parts.end(), address, [](uintptr_t address, const Part& part) {
		return address < reinterpret_cast<uintptr_t>(part.block->live.data);
	});
	if (end == m_Parts.begin() || !std::prev(end)->block->Contains(source, 1)) return nullptr;
	const auto& block = *std::prev(end)->block;
	const size_t offset = address - reinterpret_cast<uintptr_t>(block.live.data);
	const size_t slot = offset / block.slotBytes * block.slotBytes;
	const auto bytes = ReadBytes(block.live.data + slot, std::min(block.slotBytes, block.live.bytes - slot));
	return bytes.data() + offset - slot;
}

bool CheckpointPagePool::Snapshot::Contains(const void* source, size_t bytes) const {
	const uintptr_t address = reinterpret_cast<uintptr_t>(source);
	const auto end = std::upper_bound(m_Parts.begin(), m_Parts.end(), address, [](uintptr_t address, const Part& part) {
		return address < reinterpret_cast<uintptr_t>(part.block->live.data);
	});
	return end != m_Parts.begin() && std::prev(end)->block->Contains(source, bytes);
}

const void* CheckpointPagePool::Snapshot::Original(const void* view, size_t bytes) const {
	const uintptr_t address = reinterpret_cast<uintptr_t>(view);
	const auto end = std::upper_bound(m_Views.begin(), m_Views.end(), address, [this](uintptr_t address, size_t index) {
		return address < reinterpret_cast<uintptr_t>(m_Parts[index].copy->pages.data);
	});
	if (end == m_Views.begin()) return nullptr;
	const auto& part = m_Parts[*std::prev(end)];
	const uintptr_t first = reinterpret_cast<uintptr_t>(part.copy->pages.data);
	if (address - first <= part.copy->pages.bytes && bytes <= part.copy->pages.bytes - (address - first))
		return part.block->live.data + (address - first);
	return nullptr;
}

bool CheckpointPagePool::Snapshot::CanBorrow(const void* source, size_t bytes) const {
	const uintptr_t address = reinterpret_cast<uintptr_t>(source);
	const auto end = std::upper_bound(m_Parts.begin(), m_Parts.end(), address, [](uintptr_t address, const Part& part) {
		return address < reinterpret_cast<uintptr_t>(part.block->live.data);
	});
	if (end == m_Parts.begin()) return false;
	const Part& part = *std::prev(end);
	if (!part.block->Contains(source, bytes)) return false;
	if (!part.copy->Armed()) return false;
	if (!bytes) return true;
	const size_t first = (address - reinterpret_cast<uintptr_t>(part.block->live.data)) / part.block->pageBytes;
	const size_t last = (address - reinterpret_cast<uintptr_t>(part.block->live.data) + bytes - 1) / part.block->pageBytes;
	for (size_t page = first; page <= last; ++page) if (part.copy->saved[page].load(std::memory_order_acquire)) return false;
	return true;
}

CheckpointPagePool::Snapshot::~Snapshot() {
	Cancel();
}

bool CheckpointPagePool::Snapshot::Read(const void* source, void* destination, size_t bytes) const {
	const uintptr_t address = reinterpret_cast<uintptr_t>(source);
	const auto end = std::upper_bound(m_Parts.begin(), m_Parts.end(), address, [](uintptr_t address, const Part& part) {
		return address < reinterpret_cast<uintptr_t>(part.block->live.data);
	});
	if (end == m_Parts.begin()) return false;
	const Part& part = *std::prev(end);
	if (!part.block->Contains(source, bytes)) return false;
	const auto frozen = ReadBytes(source, bytes);
	if (bytes) std::memcpy(destination, frozen.data(), bytes);
	return true;
}

void CheckpointPagePool::Snapshot::Arm() const {
	if (m_ArmGate) {
		ArmGate::State expected = ArmGate::State::Prepared;
		if (!m_ArmGate->state.compare_exchange_strong(expected, ArmGate::State::Arming, std::memory_order_acq_rel)) {
			if (expected == ArmGate::State::Armed) return;
			throw std::logic_error("native checkpoint inventory is no longer prepared");
		}
		try {
			m_ArmGate->simThread = std::this_thread::get_id();
			// The written pages are copied by several threads while the simulation waits; each block arms under its own lock.
			std::atomic<size_t> next = 0, copiedPages = 0, fencedPages = 0;
			const auto arm = [&] {
				for (size_t index; (index = next.fetch_add(1, std::memory_order_relaxed)) < m_Parts.size();) {
					const auto [copied, fenced] = m_Parts[index].block->ArmInventory(*m_Parts[index].copy);
					copiedPages += copied; fencedPages += fenced;
				}
			};
			static const bool s_SerialArm = std::getenv("CCCP_TEST_CHECKPOINT_SERIAL_ARM") != nullptr;
			const unsigned helperCount = s_SerialArm ? 0 : std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u) - 2;
			std::vector<std::future<void>> helpers;
			std::exception_ptr failure;
			try {
				for (unsigned helper = 0; helper < helperCount; ++helper) helpers.push_back(std::async(std::launch::async, arm));
				arm();
			} catch (...) { failure = std::current_exception(); next.store(m_Parts.size()); }
			for (auto& helper: helpers) {
				try { helper.get(); } catch (...) { if (!failure) failure = std::current_exception(); }
			}
			if (failure) std::rethrow_exception(failure);
			m_ArmGate->copiedPages = copiedPages; m_ArmGate->fencedPages = fencedPages;
			m_ArmGate->Publish(ArmGate::State::Armed);
			return;
		} catch (...) { Cancel(); throw; }
	}
	try {
		for (const auto& part: m_Parts) part.block->Arm(part.copy);
	} catch (...) {
		Cancel();
		throw;
	}
}

std::pair<size_t, size_t> CheckpointPagePool::Snapshot::ArmedPages() const {
	return m_ArmGate && m_ArmGate->state.load(std::memory_order_acquire) == ArmGate::State::Armed ? std::pair{m_ArmGate->copiedPages, m_ArmGate->fencedPages} : std::pair<size_t, size_t>{};
}

void CheckpointPagePool::Snapshot::WarmShadow() const {
	for (const auto& part: m_Parts) {
		if (part.copy->Armed() || part.copy->abandoned)
			throw std::logic_error("native checkpoint shadow warming after its boundary");
		auto* destination = static_cast<volatile unsigned char*>(part.copy->pages.data);
		// An inventory boundary writes only the pages written since the last one; the saver fills the rest itself.
		std::vector<unsigned char> written;
		if (m_ArmGate) {
			Locked guard(part.block->lock);
			written.reserve(part.block->states.size());
			for (const auto state: part.block->states) written.push_back(state == Block::PageState::Hot);
			for (size_t page = 0; page < written.size(); ++page) if (written[page] && !part.copy->Commit(page, 1, part.block->pageBytes)) throw std::bad_alloc();
		}
		for (size_t page = 0; page < part.copy->saved.size(); ++page) if (!m_ArmGate || written[page]) destination[page * part.block->pageBytes] = 0;
	}
}

void CheckpointPagePool::Snapshot::Cancel() const noexcept {
	if (m_ArmGate) m_ArmGate->Publish(ArmGate::State::Cancelled);
	for (const auto& part: m_Parts) if (part.copy) part.block->Abandon(part.copy);
}

void CheckpointPagePool::Snapshot::Drain() const {
	for (const auto& part: m_Parts) {
		if (!part.copy->Armed()) throw std::logic_error("native checkpoint drain before its boundary");
		// Inventory pages stay fenced until written, so the saver copies only what it reads.
		if (m_ArmGate) continue;
		const auto start = RTE::SteadyClock::now();
		const size_t batch = std::max<size_t>(1, (size_t{64} << 10) / part.block->pageBytes);
		for (size_t page = 0; page < part.copy->saved.size(); page += batch) {
			if (!part.block->SavePages(page, std::min(batch, part.copy->saved.size() - page), false)) throw std::runtime_error("could not drain native checkpoint pages");
			SaverThreads::YieldToGame();
		}
		part.block->Complete(part.copy);
		part.copy->workerUs.fetch_add(Since(start), std::memory_order_relaxed);
	}
}

CheckpointPagePool::Costs CheckpointPagePool::Snapshot::Cost() const {
	Costs result;
	for (const auto& part: m_Parts) {
		result.bytes += part.copy->pages.bytes;
		result.committedBytes += part.copy->committedBytes.load(std::memory_order_relaxed);
		result.workerUs += part.copy->workerUs.load(std::memory_order_relaxed);
		result.simFaultUs += (part.copy->simFaultNs.load(std::memory_order_relaxed) + 999) / 1000;
		result.otherFaultUs += (part.copy->otherFaultNs.load(std::memory_order_relaxed) + 999) / 1000;
		result.simFaults += part.copy->simFaults.load(std::memory_order_relaxed);
		result.otherFaults += part.copy->otherFaults.load(std::memory_order_relaxed);
	}
	return result;
}

std::string CheckpointPagePool::SelfTestMismatch() {
	{
		// A prepared inventory holds address space for its shadow, never a second committed copy of the inventory.
		constexpr size_t c_Bytes = size_t{64} << 20;
		Allocation owned(c_Bytes, true);
		std::memset(owned.Data(), 1, c_Bytes);
		uint64_t generation = 0;
		const int64_t before = CommittedPrivateBytes();
		auto prepared = PrepareInventory(generation);
		prepared->WarmShadow();
		const int64_t grown = CommittedPrivateBytes() - before;
		prepared->Cancel();
		if (before >= 0 && grown > static_cast<int64_t>(c_Bytes / 4))
			return std::format("a prepared inventory committed {} MB for its {} MB of storage before any page was written", grown >> 20, c_Bytes >> 20);
	}
	{
		Allocation owned(4096, true);
		auto* value = static_cast<uint64_t*>(owned.Data());
		*value = 211;
		uint64_t generation = 0;
		auto first = PrepareInventory(generation);
		first->Arm();
		*value = 223;
		first->Drain();
		auto next = PrepareInventory(generation);
		next->m_ArmGate->Publish(ArmGate::State::Arming);
		for (const auto& range: next->m_Ranges) if (!PageWriteFence::ProtectCopyPages(range.first, range.bytes)) {
			next->Cancel();
			return "prepared inventory could not protect its ranges";
		}
		for (const auto& part: first->m_Parts) part.block->Complete(part.copy);
		first->Cancel();
		next->m_ArmGate->simThread = std::this_thread::get_id();
		next->m_ArmGate->Publish(ArmGate::State::Armed);
		*value = 227;
		next->Drain();
		uint64_t saved = 0;
		if (!next->Read(value, &saved, sizeof(saved)) || saved != 223)
			return "retired native inventory opened the next generation's arming range";
		next->Cancel();
		bool refused = false;
		try { next->Arm(); } catch (const std::logic_error&) { refused = true; }
		if (!refused) return "cancelled inventory was armed again";
	}
	{
		std::shared_ptr<const Snapshot> snapshot;
		const uint64_t* source = nullptr;
		{
			Allocation owned(4096, true), transient(4096);
			auto* values = static_cast<uint64_t*>(owned.Data());
			values[0] = 113; values[511] = 997;
			source = values;
			uint64_t generation = 0;
			{
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativePages);
				bool refused = false;
				try { PrepareInventory(generation); } catch (const std::bad_alloc&) { refused = true; }
				if (!refused) return "inventory ignored an injected shadow allocation failure";
			}
			values[0] = 127;
			snapshot = std::async(std::launch::async, [&] {
				auto prepared = PrepareInventory(generation); prepared->WarmShadow(); return prepared;
			}).get();
			if (generation != InventoryGeneration() || !snapshot->Contains(values, 4096) || snapshot->Contains(transient.Data(), 4096))
				return "owned page inventory included transient storage or lost a registered range";
			snapshot->Arm();
			bool refused = false;
			try { snapshot->WarmShadow(); } catch (const std::logic_error&) { refused = true; }
			if (!refused) return "armed native shadow accepted a preparation write";
			values[0] = 149; values[511] = 991;
		}
		snapshot->Drain();
		std::array<uint64_t, 512> values{};
		if (!snapshot->Read(source, values.data(), sizeof(values)) || values[0] != 127 || values[511] != 997)
			return "registered allocation destruction changed its frozen range";
		const void* view = snapshot->ReadAllocation(source);
		if (snapshot->Original(view) != source || snapshot->Original(static_cast<const std::byte*>(view) + 4088) != source + 511)
			return "frozen range index lost its canonical address";
	}
	std::shared_ptr<const Snapshot> first, second;
	std::vector<unsigned char> expected(20003), restored(expected.size());
	for (size_t byte = 0; byte < expected.size(); ++byte) expected[byte] = static_cast<unsigned char>(byte * 37);
	void* source;
	{
		CheckpointPagePool pool;
		std::vector<void*> free;
		pool.Grow(expected.size() + 37, 2, free);
		source = static_cast<unsigned char*>(free.back()) + 17;
		std::memcpy(source, expected.data(), expected.size());
		pool.Grow(expected.size() + 37, 2, free);
		for (size_t after: {size_t{0}, size_t{1}}) {
			bool refused = false;
			{
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativePages, after);
				try { pool.Freeze(); } catch (const std::bad_alloc&) { refused = true; }
			}
			if (!refused || std::memcmp(source, expected.data(), expected.size())) return "native page allocation failure changed live bytes";
			std::memset(source, 71, expected.size());
			std::memcpy(source, expected.data(), expected.size());
		}
		auto prepared = std::async(std::launch::async, [&] { return std::pair{pool.Freeze(false), pool.Freeze(false)}; }).get();
		first = std::move(prepared.first);
		second = std::move(prepared.second);
		bool refusedRead = false, refusedDrain = false;
		try { first->Read(source, restored.data(), restored.size()); } catch (const std::logic_error&) { refusedRead = true; }
		try { first->Drain(); } catch (const std::logic_error&) { refusedDrain = true; }
		if (!refusedRead || !refusedDrain || first->CanBorrow(source, expected.size())) return "prepared native pages became readable before the boundary";
		std::memset(source, 23, expected.size());
		std::memcpy(source, expected.data(), expected.size());
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::NativePages);
			first->Arm();
		}
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::CopyWatch);
			first->Arm();
		}
		if (!first->CanBorrow(source, expected.size())) return "fresh native pages could not be borrowed";
		std::memset(source, 71, expected.size());
		if (first->CanBorrow(source, expected.size())) return "changed native pages were still borrowed";
		second->Arm();
		std::memset(source, 99, expected.size());
		if (!first->Read(source, restored.data(), restored.size()) || restored != expected) return "first native page generation differs";
	}
	auto drain = std::async(std::launch::async, [&] { first->Drain(); second->Drain(); });
	drain.get();
	if (!first->Contains(source, expected.size()) || first->CanBorrow(source, expected.size()) ||
	    !first->Read(source, restored.data(), restored.size()) || restored != expected)
		return "drained native pages lost their frozen ownership";
	if (!second->Read(source, restored.data(), restored.size()) || !std::all_of(restored.begin(), restored.end(), [](auto byte) { return byte == 71; }))
		return "native page snapshot did not survive its pool";
	if (first->Read(expected.data(), restored.data(), restored.size())) return "native page snapshot accepted an unrelated address";
	return {};
}

namespace {
	constexpr size_t c_NativeStorageBlockBytes = size_t{1} << 20;
	struct NativeStoragePool {
		CheckpointPagePool pages;
		std::vector<void*> free;
		uint64_t bytes = 0;
		NativeStoragePool() { pages.IncludeInInventory(); }
	};
	struct NativeStorageRange {
		size_t bytes = 0;
		NativeStoragePool* pool = nullptr;
	};
	struct NativeStoragePools {
		std::mutex mutex;
		std::map<size_t, NativeStoragePool> sizes;
		std::map<uintptr_t, NativeStorageRange> ranges;
		std::map<uintptr_t, std::unique_ptr<CheckpointPagePool::Allocation>> large;
		uint64_t largeBytes = 0, peakLargeBytes = 0, releasedLargeBytes = 0;
		uint64_t explicitGrownBytes = 0, worldGrownBytes = 0;
	};
	std::atomic<NativeStoragePools*> s_NativeStoragePools{nullptr};
	struct StorageCostCounters {
		std::atomic<uint64_t> allocations{0}, allocateNs{0}, frees{0}, freeNs{0}, grows{0}, growNs{0}, larges{0};
	};
	StorageCostCounters* StorageCostsArmed() {
		static StorageCostCounters* const counters = std::getenv("CCCP_TEST_STORAGE_COSTS") ? new StorageCostCounters : nullptr;
		return counters;
	}
	struct StorageCostSpan {
		std::atomic<uint64_t>* count = nullptr;
		std::atomic<uint64_t>* ns = nullptr;
		RTE::SteadyClock::time_point started;
		StorageCostSpan(std::atomic<uint64_t> StorageCostCounters::*countField, std::atomic<uint64_t> StorageCostCounters::*nsField) {
			if (auto* counters = StorageCostsArmed()) { count = &(counters->*countField); ns = &(counters->*nsField); started = RTE::SteadyClock::now(); }
		}
		~StorageCostSpan() {
			if (!count) return;
			count->fetch_add(1, std::memory_order_relaxed);
			ns->fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(RTE::SteadyClock::now() - started).count(), std::memory_order_relaxed);
		}
	};
	thread_local bool s_NativeStorageAllocation = false;
	thread_local bool s_NativeStorageCapture = false;
	thread_local std::optional<bool> s_NativeStorageWorldAllocation;
	std::atomic<bool> s_NativeStorageWorldEnabled{false};
	std::atomic<int> s_CoolingPauses{0};
	NativeStoragePools& NativePools() {
		static NativeStoragePools* const pools = [] {
			auto* value = new NativeStoragePools;
			s_NativeStoragePools.store(value, std::memory_order_release);
			return value;
		}();
		return *pools;
	}
}

CheckpointNativeStorage::AllocationScope::AllocationScope(bool enabled) : m_Previous(s_NativeStorageAllocation) {
	s_NativeStorageAllocation = s_NativeStorageAllocation || enabled;
}

CheckpointNativeStorage::AllocationScope::~AllocationScope() { s_NativeStorageAllocation = m_Previous; }

CheckpointNativeStorage::CaptureScope::CaptureScope(bool enabled) : m_Previous(s_NativeStorageCapture) {
	s_NativeStorageCapture = s_NativeStorageCapture || enabled;
}

CheckpointNativeStorage::CaptureScope::~CaptureScope() { s_NativeStorageCapture = m_Previous; }

CheckpointNativeStorage::WorldAllocationScope::WorldAllocationScope(bool enabled) : m_Previous(s_NativeStorageWorldAllocation) {
	s_NativeStorageWorldAllocation = enabled;
}

CheckpointNativeStorage::WorldAllocationScope::~WorldAllocationScope() { s_NativeStorageWorldAllocation = m_Previous; }

bool CheckpointNativeStorage::WorldEnabled() noexcept { return s_NativeStorageWorldEnabled.load(std::memory_order_acquire); }
bool CheckpointNativeStorage::SetWorldEnabled(bool enabled) noexcept { return s_NativeStorageWorldEnabled.exchange(enabled, std::memory_order_acq_rel); }

CheckpointNativeStorage::CoolingPause::CoolingPause() { s_CoolingPauses.fetch_add(1, std::memory_order_acq_rel); }
CheckpointNativeStorage::CoolingPause::~CoolingPause() { s_CoolingPauses.fetch_sub(1, std::memory_order_acq_rel); }

void CheckpointNativeStorage::StartSettling() {
	// Faults on the newly fenced pages are spread over the slices instead of landing on one tick.
	static std::jthread settler([](std::stop_token stop) {
		SaverThreads::Mark();
		uint64_t settled = UINT64_MAX;
		auto cooled = RTE::SteadyClock::now();
		while (!stop.stop_requested()) {
			const uint64_t generation = CheckpointPagePool::InventoryGeneration();
			bool more = false;
			if (WorldEnabled() && generation != settled) {
				more = CheckpointPagePool::SettleInventory(2048) > 0;
				if (!more) settled = generation;
			}
			// A page that stopped changing for a pass is fenced again, so the next boundary copies only pages still written.
			static const bool s_Cooling = std::getenv("CCCP_TEST_CHECKPOINT_NO_COOLING") == nullptr;
			if (s_Cooling && WorldEnabled() && s_CoolingPauses.load(std::memory_order_acquire) == 0 && RTE::SteadyClock::now() - cooled >= std::chrono::seconds(1)) {
				CheckpointPagePool::CoolInventory();
				cooled = RTE::SteadyClock::now();
			}
			std::this_thread::sleep_for(more ? std::chrono::milliseconds(4) : std::chrono::milliseconds(50));
		}
	});
}

bool CheckpointNativeStorage::RetainingAllocations() noexcept {
	return !s_NativeStorageCapture && (s_NativeStorageAllocation || s_NativeStorageWorldAllocation.value_or(WorldEnabled()));
}

bool CheckpointNativeStorage::Enabled(bool retained) {
	if (s_NativeStorageCapture) return false;
	if (retained || s_NativeStorageAllocation) return true;
	if (s_NativeStorageWorldAllocation) return *s_NativeStorageWorldAllocation;
	return WorldEnabled() || ScenarioRunner::HasLockstepCoordinator();
}

void* CheckpointNativeStorage::Allocate(size_t bytes, size_t alignment) {
	const StorageCostSpan cost(&StorageCostCounters::allocations, &StorageCostCounters::allocateNs);
	const size_t requested = std::max({bytes, alignment, size_t{16}});
	if (requested > (size_t{1} << (std::numeric_limits<size_t>::digits - 1)) ||
	    !std::has_single_bit(alignment) || alignment > PageWriteFence::SystemPageBytes()) throw std::bad_alloc();
	const size_t step = std::max({std::bit_floor(requested) / 8, alignment, size_t{16}});
	const size_t slot = (requested + step - 1) / step * step;
	auto& pools = NativePools();
	std::lock_guard lock(pools.mutex);
	if (requested >= c_NativeStorageBlockBytes) {
		if (auto* counters = StorageCostsArmed()) counters->larges.fetch_add(1, std::memory_order_relaxed);
		auto allocation = std::make_unique<CheckpointPagePool::Allocation>(requested);
		void* address = allocation->Data();
		const auto [entry, inserted] = pools.large.emplace(reinterpret_cast<uintptr_t>(address), std::move(allocation));
		if (!inserted) throw std::logic_error("native allocation overlaps a live buffer");
		try { entry->second->IncludeInInventory(); }
		catch (...) { pools.large.erase(entry); throw; }
		pools.largeBytes += entry->second->Bytes();
		pools.peakLargeBytes = std::max(pools.peakLargeBytes, pools.largeBytes);
		(s_NativeStorageAllocation ? pools.explicitGrownBytes : pools.worldGrownBytes) += entry->second->Bytes();
		return address;
	}
	auto& pool = pools.sizes.try_emplace(slot).first->second;
	if (pool.free.empty()) {
		// Prepare the address index before growth, so returning a live slot never allocates.
		const auto staging = pools.ranges.emplace(0, NativeStorageRange{}).first;
		auto node = pools.ranges.extract(staging);
		const StorageCostSpan growth(&StorageCostCounters::grows, &StorageCostCounters::growNs);
		const auto [address, bytes] = pool.pages.Grow(slot, 1, pool.free, c_NativeStorageBlockBytes);
		node.key() = reinterpret_cast<uintptr_t>(address);
		node.mapped() = {bytes, &pool};
		pools.ranges.insert(std::move(node));
		pool.bytes += pool.free.size() * slot;
		(s_NativeStorageAllocation ? pools.explicitGrownBytes : pools.worldGrownBytes) += bytes;
	}
	void* address = pool.free.back();
	pool.free.pop_back();
	return address;
}

bool CheckpointNativeStorage::Deallocate(void* address) noexcept {
	if (!address) return false;
	const StorageCostSpan cost(&StorageCostCounters::frees, &StorageCostCounters::freeNs);
	if (ForgetPrivate(address)) return false;
	auto* pools = s_NativeStoragePools.load(std::memory_order_acquire);
	if (!pools) return false;
	std::lock_guard lock(pools->mutex);
	if (const auto found = pools->large.find(reinterpret_cast<uintptr_t>(address)); found != pools->large.end()) {
		const size_t bytes = found->second->Bytes();
		// Frozen readers retain these pages after their live owner releases them.
		found->second->ExcludeFromInventory();
		pools->large.erase(found);
		pools->largeBytes -= bytes;
		pools->releasedLargeBytes += bytes;
		static const bool report = [] { const char* value = std::getenv("CCCP_CHECKPOINT_ALLOCATIONS"); return value && std::string_view(value) == "1"; }();
		if (report) try {
			static bool sizesReported = false;
			uint64_t retained = 0, free = 0;
			for (const auto& [slot, pool]: pools->sizes) {
				retained += pool.bytes;
				free += pool.free.size() * slot;
				if (!sizesReported && pool.bytes) std::cout << "[checkpoint-storage-size] slot_bytes=" << slot
					<< " retained_bytes=" << pool.bytes << " free_bytes=" << pool.free.size() * slot << std::endl;
			}
			sizesReported = true;
			std::cout << "[checkpoint-storage] released_large_bytes=" << pools->releasedLargeBytes << " live_large_bytes=" << pools->largeBytes
			          << " peak_large_bytes=" << pools->peakLargeBytes << " retained_slots_bytes=" << retained << " free_slots_bytes=" << free << std::endl;
		} catch (...) {}
		return true;
	}
	const uintptr_t value = reinterpret_cast<uintptr_t>(address);
	const auto after = pools->ranges.upper_bound(value);
	if (after != pools->ranges.begin()) {
		const auto& range = *std::prev(after);
		if (value - range.first >= range.second.bytes) return false;
		// Free lists reserve every slot at growth, so returning storage cannot allocate or discard frozen pages.
		range.second.pool->free.push_back(address);
		return true;
	}
	return false;
}

std::string CheckpointNativeStorage::ThreadStackSelfTestMismatch() {
	constexpr size_t stackBytes = 96 * 1024, roomBytes = 64 * 1024;
	size_t room = 0;
#ifdef _WIN32
	const auto measure = +[](void* context) -> DWORD {
		volatile char marker = 0;
		ULONG_PTR low = 0, high = 0;
		GetCurrentThreadStackLimits(&low, &high);
		*static_cast<size_t*>(context) = reinterpret_cast<uintptr_t>(&marker) - low;
		return 0;
	};
	HANDLE thread = CreateThread(nullptr, stackBytes, measure, &room, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	if (!thread) return "could not start a thread with a 96 KB stack";
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
#else
	const auto measure = +[](void* context) -> void* {
		volatile char marker = 0;
		uintptr_t low = 0;
#ifdef __APPLE__
		low = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self())) - pthread_get_stacksize_np(pthread_self());
#else
		pthread_attr_t own;
		if (pthread_getattr_np(pthread_self(), &own) == 0) {
			void* address = nullptr;
			size_t size = 0;
			if (pthread_attr_getstack(&own, &address, &size) == 0) low = reinterpret_cast<uintptr_t>(address);
			pthread_attr_destroy(&own);
		}
#endif
		*static_cast<size_t*>(context) = low ? reinterpret_cast<uintptr_t>(&marker) - low : 0;
		return nullptr;
	};
	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, stackBytes);
	pthread_t thread;
	const int created = pthread_create(&thread, &attributes, measure, &room);
	pthread_attr_destroy(&attributes);
	if (created != 0) return std::format("could not start a thread with a 96 KB stack: error {}", created);
	pthread_join(thread, nullptr);
#endif
	if (room < roomBytes) return std::format("a thread with a 96 KB stack keeps {} bytes below its first frame", room);
	return {};
}

std::string CheckpointNativeStorage::InventoryCensus() {
	auto* pools = s_NativeStoragePools.load(std::memory_order_acquire);
	if (!pools) return "pool_bytes=0 free_slot_bytes=0 large_bytes=0 explicit_grown_bytes=0 world_grown_bytes=0";
	std::lock_guard lock(pools->mutex);
	uint64_t retained = 0, free = 0;
	for (const auto& [slot, pool]: pools->sizes) { retained += pool.bytes; free += pool.free.size() * slot; }
	return std::format("pool_bytes={} free_slot_bytes={} large_bytes={} explicit_grown_bytes={} world_grown_bytes={}",
	    retained, free, pools->largeBytes, pools->explicitGrownBytes, pools->worldGrownBytes);
}

std::string CheckpointNativeStorage::StorageCosts() {
	const auto* counters = StorageCostsArmed();
	if (!counters) return {};
	const auto ms = [](const std::atomic<uint64_t>& ns) { return ns.load() / 1000000; };
	return std::format("allocations={} allocate_ms={} frees={} free_ms={} grows={} grow_ms={} larges={} {}", counters->allocations.load(), ms(counters->allocateNs),
	    counters->frees.load(), ms(counters->freeNs), counters->grows.load(), ms(counters->growNs), counters->larges.load(), InventoryCensus());
}

bool CheckpointNativeStorage::Owns(const void* address) {
	if (Private(address)) return false;
	auto* pools = s_NativeStoragePools.load(std::memory_order_acquire);
	if (!pools) return false;
	std::lock_guard lock(pools->mutex);
	const auto end = pools->large.upper_bound(reinterpret_cast<uintptr_t>(address));
	if (end != pools->large.begin() && std::prev(end)->second->Contains(address, 1)) return true;
	const uintptr_t value = reinterpret_cast<uintptr_t>(address);
	const auto after = pools->ranges.upper_bound(value);
	return after != pools->ranges.begin() && value - std::prev(after)->first < std::prev(after)->second.bytes;
}

std::unique_ptr<CheckpointNativeStorage::Prepared> CheckpointNativeStorage::PrepareInventory() {
	auto prepared = std::make_unique<Prepared>();
	prepared->m_Pages.push_back(CheckpointPagePool::PrepareInventory(prepared->m_Generation));
	return prepared;
}

bool CheckpointNativeStorage::Prepared::Current() const noexcept {
	return m_Generation == CheckpointPagePool::InventoryGeneration();
}

bool CheckpointNativeStorage::Prepared::Arm() const {
	// Growth after the worker's inventory must refuse before any range becomes readable.
	if (!Current()) return false;
	try {
		for (const auto& pages: m_Pages) pages->Arm();
	} catch (...) {
		for (const auto& pages: m_Pages) pages->Cancel();
		throw;
	}
	if (Current()) return true;
	for (const auto& pages: m_Pages) pages->Cancel();
	return false;
}

void CheckpointNativeStorage::Prepared::Cancel() const noexcept {
	for (const auto& pages: m_Pages) pages->Cancel();
}

std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> CheckpointNativeStorage::Prepare() {
	return std::move(PrepareInventory()->m_Pages);
}

struct CheckpointNativeStorage::PrivateValues {
	std::mutex mutex;
	struct Range { size_t bytes; bool borrowed; };
	std::map<uintptr_t, Range> ranges;
};

namespace {
	// Overlapping roots keep the first recorded owner that covers the requested bytes.
	class CheckpointRootIndex {
	public:
		using Root = CheckpointNativeStorage::RootRange;
		void Build(std::span<const Root> roots, bool views, bool privateOnly = false) {
			static std::atomic<uint64_t> nextGeneration{1};
			m_Generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
			if (!roots.empty()) CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
			m_Entries.reserve(roots.size());
			for (size_t index = 0; index < roots.size(); ++index) {
				const Root& root = roots[index];
				if (privateOnly && !root.privateValues) continue;
				const auto start = reinterpret_cast<uintptr_t>(views ? root.view : root.source);
				m_Entries.push_back({start, End(start, root.bytes), index, index});
			}
			size_t compared = 0;
			std::sort(m_Entries.begin(), m_Entries.end(), [&compared](const Entry& left, const Entry& right) {
				if (++compared % 4096 == 0) SaverThreads::YieldToGame();
				return left.start < right.start || (left.start == right.start && left.root < right.root);
			});
			Build(0, m_Entries.size());
		}
		const Root* Find(std::span<const Root> roots, const void* pointer, size_t bytes) const noexcept {
			if (m_Entries.empty()) return nullptr;
			const auto address = reinterpret_cast<uintptr_t>(pointer);
			// A lookup is immutable. Cache exact queries, including misses, without sharing writable state
			// between readers. The generation keeps recycled index addresses from reusing an older answer.
			struct Cached { uint64_t generation; uintptr_t address; size_t bytes, root; };
			// On the heap: glibc places static thread-locals at the top of every thread's stack, a library's small ones too.
			static thread_local std::unique_ptr<std::array<Cached, 128>> cache;
			if (!cache) cache.reset(new (std::nothrow) std::array<Cached, 128>{});
			size_t found = c_None;
			if (!cache) {
				Find(roots, 0, m_Entries.size(), address, End(address, bytes), bytes, found);
				return found == c_None ? nullptr : &roots[found];
			}
			const size_t slot = ((address >> 4) ^ (address >> 17) ^ (bytes * 37) ^ m_Generation) & (cache->size() - 1);
			Cached& cached = (*cache)[slot];
			if (cached.generation == m_Generation && cached.address == address && cached.bytes == bytes)
				return cached.root == c_None ? nullptr : &roots[cached.root];
			Find(roots, 0, m_Entries.size(), address, End(address, bytes), bytes, found);
			cached = {m_Generation, address, bytes, found};
			return found == c_None ? nullptr : &roots[found];
		}
	private:
		static constexpr size_t c_None = (std::numeric_limits<size_t>::max)();
		struct Entry { uintptr_t start, last; size_t root, firstRoot; };
		std::vector<Entry> m_Entries;
		uint64_t m_Generation = 0;
		static uintptr_t End(uintptr_t start, size_t bytes) noexcept {
			return start + (std::min<uintptr_t>)(bytes, (std::numeric_limits<uintptr_t>::max)() - start);
		}
		void Build(size_t first, size_t last) {
			if (first == last) return;
			const size_t middle = first + (last - first) / 2;
			Build(first, middle); Build(middle + 1, last);
			Entry& entry = m_Entries[middle];
			const auto include = [&](size_t begin, size_t end) {
				if (begin == end) return;
				const Entry& child = m_Entries[begin + (end - begin) / 2];
				entry.last = (std::max)(entry.last, child.last);
				entry.firstRoot = (std::min)(entry.firstRoot, child.firstRoot);
			};
			include(first, middle); include(middle + 1, last);
		}
		void Find(std::span<const Root> roots, size_t first, size_t last, uintptr_t address, uintptr_t end, size_t bytes, size_t& found) const noexcept {
			if (first == last || m_Entries[first].start > address) return;
			const size_t middle = first + (last - first) / 2;
			const Entry& entry = m_Entries[middle];
			if (entry.last < end || entry.firstRoot >= found) return;
			const Root& root = roots[entry.root];
			if (entry.root < found && address >= entry.start && address - entry.start <= root.bytes && bytes <= root.bytes - (address - entry.start))
				found = entry.root;
			const size_t left = first == middle ? c_None : m_Entries[first + (middle - first) / 2].firstRoot;
			const size_t right = middle + 1 == last ? c_None : m_Entries[middle + 1 + (last - middle - 1) / 2].firstRoot;
			if (left <= right) {
				Find(roots, first, middle, address, end, bytes, found);
				Find(roots, middle + 1, last, address, end, bytes, found);
			} else {
				Find(roots, middle + 1, last, address, end, bytes, found);
				Find(roots, first, middle, address, end, bytes, found);
			}
		}
	};
}

// The pages, roots and root indices a read scope looks through; they never change once built, so the scopes a saver's
// helpers open on its views share them instead of copying and sorting every root again.
struct CheckpointNativeStorage::ReadLookup {
	std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> pages;
	std::vector<RootRange> roots;
	CheckpointRootIndex sourceRoots, viewRoots, privateRoots;
};

struct CheckpointNativeStorage::ReadState : std::enable_shared_from_this<ReadState> {
	explicit ReadState(std::shared_ptr<const ReadLookup> built) :
	    lookup(std::move(built)), pages(lookup->pages), roots(lookup->roots), sourceRoots(lookup->sourceRoots), viewRoots(lookup->viewRoots), privateRoots(lookup->privateRoots) {}
	std::shared_ptr<const void> sourceOwner;
	std::shared_ptr<const ReadLookup> lookup;
	const std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>>& pages;
	const std::vector<RootRange>& roots;
	const CheckpointRootIndex& sourceRoots;
	const CheckpointRootIndex& viewRoots;
	const CheckpointRootIndex& privateRoots;
	std::map<std::pair<uintptr_t, size_t>, std::string> strings;
	std::map<std::pair<uintptr_t, uintptr_t>, std::shared_ptr<const void>> indices;
	std::shared_ptr<PrivateValues> privateValues;
};

thread_local CheckpointNativeStorage::ReadState* CheckpointNativeStorage::s_Read = nullptr;

CheckpointNativeStorage::ReadScope::ReadScope(std::span<const std::shared_ptr<const CheckpointPagePool::Snapshot>> pages, std::span<const RootRange> roots)
	: ReadScope(ReadViews{true, pages, roots}) {}

CheckpointNativeStorage::ReadScope::ReadScope(ReadViews views) : m_Capture(views.active), m_Previous(s_Read) {
	if (!views.active) return;
	std::shared_ptr<const ReadLookup> lookup = std::move(views.lookup);
	if (lookup && lookup->pages.data() == views.pages.data() && lookup->pages.size() == views.pages.size() && lookup->roots.data() == views.roots.data() && lookup->roots.size() == views.roots.size()) {
		// The three indices count as built here, so an injected failure meets the same points either way.
		if (!views.roots.empty()) for (int index = 0; index < 3; ++index) CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
	} else {
		auto built = std::make_shared<ReadLookup>();
		built->pages.assign(views.pages.begin(), views.pages.end());
		built->roots.assign(views.roots.begin(), views.roots.end());
		built->sourceRoots.Build(built->roots, false);
		built->viewRoots.Build(built->roots, true);
		built->privateRoots.Build(built->roots, true, true);
		lookup = std::move(built);
	}
	m_State = std::make_shared<ReadState>(std::move(lookup));
	m_State->sourceOwner = std::move(views.owner);
	m_State->privateValues = views.privateValues ? std::move(views.privateValues) : (m_Previous ? m_Previous->privateValues : std::make_shared<PrivateValues>());
	s_Read = m_State.get();
}

void CheckpointNativeStorage::RememberPrivate(void* address, size_t bytes, bool borrowed) {
	if (!s_Read || !address) throw std::logic_error("private native values need their saver scope");
	CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
	std::lock_guard lock(s_Read->privateValues->mutex);
	if (!s_Read->privateValues->ranges.emplace(reinterpret_cast<uintptr_t>(address), PrivateValues::Range{bytes, borrowed}).second)
		throw std::logic_error("private native storage is already recorded");
}

bool CheckpointNativeStorage::ForgetPrivate(void* address) noexcept {
	return s_Read && ForgetPrivate(s_Read->privateValues, address);
}

bool CheckpointNativeStorage::ForgetPrivate(const std::shared_ptr<PrivateValues>& values, void* address) noexcept {
	if (!values) return false;
	std::lock_guard lock(values->mutex);
	return values->ranges.erase(reinterpret_cast<uintptr_t>(address)) != 0;
}

size_t CheckpointNativeStorage::PrivateAllocationCount() noexcept {
	if (!s_Read) return 0;
	std::lock_guard lock(s_Read->privateValues->mutex);
	return s_Read->privateValues->ranges.size();
}

const void* CheckpointNativeStorage::PrivateAllocation(const void* address) noexcept {
	if (!s_Read || !address) return nullptr;
	const uintptr_t pointer = reinterpret_cast<uintptr_t>(address);
	std::lock_guard lock(s_Read->privateValues->mutex);
	const auto& ranges = s_Read->privateValues->ranges;
	const auto after = ranges.upper_bound(pointer);
	if (after == ranges.begin()) return nullptr;
	const auto value = std::prev(after);
	return pointer - value->first < value->second.bytes ? reinterpret_cast<const void*>(value->first) : nullptr;
}

void CheckpointNativeStorage::BorrowPrivate(const void* address) noexcept {
	if (!s_Read || !address) return;
	const auto pointer = reinterpret_cast<uintptr_t>(address);
	std::lock_guard lock(s_Read->privateValues->mutex);
	auto& ranges = s_Read->privateValues->ranges;
	const auto after = ranges.upper_bound(pointer);
	if (after != ranges.begin()) {
		const auto value = std::prev(after);
		if (pointer - value->first < value->second.bytes) value->second.borrowed = true;
	}
}

bool CheckpointNativeStorage::PrivateBorrowed(const void* address) noexcept {
	if (!s_Read || !address) return false;
	const auto pointer = reinterpret_cast<uintptr_t>(address);
	{
		std::lock_guard lock(s_Read->privateValues->mutex);
		const auto& ranges = s_Read->privateValues->ranges;
		const auto after = ranges.upper_bound(pointer);
		if (after != ranges.begin()) {
			const auto value = std::prev(after);
			if (pointer - value->first < value->second.bytes) return value->second.borrowed;
		}
	}
	return s_Read->privateRoots.Find(s_Read->roots, address, 1) != nullptr;
}

bool CheckpointNativeStorage::Private(const void* address, size_t bytes) noexcept {
	if (!s_Read || !address) return false;
	const uintptr_t pointer = reinterpret_cast<uintptr_t>(address);
	{
		std::lock_guard lock(s_Read->privateValues->mutex);
		const auto& ranges = s_Read->privateValues->ranges;
		const auto after = ranges.upper_bound(pointer);
		if (after != ranges.begin()) {
			const auto value = std::prev(after);
			if (pointer - value->first < value->second.bytes && bytes <= value->second.bytes - (pointer - value->first)) return true;
		}
	}
	return s_Read->privateRoots.Find(s_Read->roots, address, bytes) != nullptr;
}

void* CheckpointNativeStorage::AllocatePrivate(size_t bytes, size_t alignment, bool aligned) {
	aligned = aligned || alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
	void* address = aligned ? ::operator new(bytes, std::align_val_t(alignment)) : ::operator new(bytes);
	try { RememberPrivate(address, bytes); }
	catch (...) {
		if (aligned) ::operator delete(address, std::align_val_t(alignment)); else ::operator delete(address);
		throw;
	}
	return address;
}

CheckpointNativeStorage::ReadViews CheckpointNativeStorage::CurrentViews() {
	return s_Read ? ReadViews{true, s_Read->pages, s_Read->roots, s_Read->privateValues, s_Read->shared_from_this(), s_Read->lookup} : ReadViews{};
}

std::shared_ptr<const void> CheckpointNativeStorage::ReadIndex(const void* source, const void* kind, const std::function<std::shared_ptr<const void>()>& build) {
	if (!s_Read) throw std::logic_error("a frozen index needs its snapshot scope");
	const auto key = std::pair{reinterpret_cast<uintptr_t>(Original(source)), reinterpret_cast<uintptr_t>(kind)};
	if (const auto found = s_Read->indices.find(key); found != s_Read->indices.end()) return found->second;
	return s_Read->indices.emplace(key, build()).first->second;
}

const void* CheckpointNativeStorage::RootView(const void* source, size_t bytes) {
	if (!s_Read || !source) return nullptr;
	source = Original(source);
	const uintptr_t address = reinterpret_cast<uintptr_t>(source);
	if (const RootRange* root = s_Read->sourceRoots.Find(s_Read->roots, source, bytes))
		return static_cast<const std::byte*>(root->view) + (address - reinterpret_cast<uintptr_t>(root->source));
	return nullptr;
}

CheckpointNativeStorage::ReadScope::~ReadScope() { s_Read = m_Previous; }

bool CheckpointNativeStorage::ReadBytes(const void* source, void* target, size_t bytes) {
	SaverThreads::YieldToGame();
	if (Private(source, bytes)) { std::memcpy(target, source, bytes); return true; }
	source = Original(source);
	if (const void* root = RootView(source, bytes)) { std::memcpy(target, root, bytes); return true; }
	if (s_Read) for (const auto& pages: s_Read->pages) if (pages->Read(source, target, bytes)) return true;
	return false;
}

const void* CheckpointNativeStorage::Original(const void* view) {
	if (s_Read && view) if (const RootRange* root = s_Read->viewRoots.Find(s_Read->roots, view, 1))
		return static_cast<const std::byte*>(root->source) + (reinterpret_cast<uintptr_t>(view) - reinterpret_cast<uintptr_t>(root->view));
	if (s_Read) for (const auto& pages: s_Read->pages) if (const void* source = pages->Original(view)) return source;
	return view;
}

bool CheckpointNativeStorage::IsView(const void* address) {
	if (s_Read && address && s_Read->privateRoots.Find(s_Read->roots, address, 1)) return false;
	return Original(address) != address;
}

bool CheckpointNativeStorage::InBoundaryRoot(const void* view) {
	if (!s_Read || !view) return false;
	const RootRange* root = s_Read->viewRoots.Find(s_Read->roots, view, 1);
	return root && !root->privateValues;
}

namespace {
	std::atomic<int> s_StartupLinksRefusals{0};
}

CheckpointNativeStorage::StartupLinksRefusal::StartupLinksRefusal() { s_StartupLinksRefusals.fetch_add(1, std::memory_order_acq_rel); }

CheckpointNativeStorage::StartupLinksRefusal::~StartupLinksRefusal() { s_StartupLinksRefusals.fetch_sub(1, std::memory_order_acq_rel); }

void CheckpointNativeStorage::NoteStartupLinks(const void* view, const char* type) {
	const RootRange* root = s_Read ? s_Read->viewRoots.Find(s_Read->roots, view, 1) : nullptr;
	std::string name = type;
#ifndef _MSC_VER
	int status = 0;
	if (char* demangled = abi::__cxa_demangle(type, nullptr, nullptr, &status)) { if (status == 0) name = demangled; std::free(demangled); }
#endif
	const size_t offset = root ? reinterpret_cast<uintptr_t>(view) - reinterpret_cast<uintptr_t>(root->view) : 0;
	const std::string message = std::format("a startup object's container keeps its links outside checkpoint storage ({} at offset {} of a {}-byte root)",
	    name, offset, root ? root->bytes : 0);
	static const bool refuse = [] { const char* value = std::getenv("CC_TEST_CHECKPOINT_STARTUP_LINKS"); return value && std::string_view(value) == "1"; }();
	if (refuse || s_StartupLinksRefusals.load(std::memory_order_acquire) > 0) throw std::logic_error(message);
	static std::mutex mutex;
	static std::set<std::pair<size_t, std::string>> named;
	{
		std::lock_guard lock(mutex);
		if (!named.emplace(offset, name).second) return;
	}
	System::PrintDiagnosticLine("[checkpoint] " + message);
}

namespace {
	// The address, the size and what this thread was freezing, so a refused capture names the value it could not read.
	std::string OutsideInventory(const char* what, const void* source, size_t bytes) {
		std::string message = std::format("native {} is outside the frozen inventory (address {:#x}, {} bytes", what, reinterpret_cast<uintptr_t>(source), bytes);
		if (const std::string chain = CheckpointCloneCost::DescribeOpen(); !chain.empty()) message += ", freezing " + chain;
		return message + ")";
	}
}

const void* CheckpointNativeStorage::View(const void* source, size_t bytes) {
	SaverThreads::YieldToGame();
	if (!s_Read || !source) return source;
	if (Private(source, bytes)) return source;
	if (const void* root = RootView(source, bytes)) return root;
	for (const auto& pages: s_Read->pages) if (pages->Original(source, bytes)) return source;
	for (const auto& pages: s_Read->pages) if (pages->Contains(source, bytes)) return pages->ReadBytes(source, bytes).data();
	throw std::logic_error(OutsideInventory("value", source, bytes));
}

std::shared_ptr<const CheckpointPagePool::Snapshot> CheckpointNativeStorage::PagesFor(const void* source, size_t bytes) {
	source = Original(source);
	if (s_Read) for (const auto& pages: s_Read->pages) if (pages->Contains(source, bytes)) return pages;
	return {};
}

const void* CheckpointNativeStorage::ViewObject(const void* source) {
	SaverThreads::YieldToGame();
	if (!s_Read || !source) return source;
	if (Private(source)) return source;
	source = Original(source);
	if (const void* root = RootView(source)) return root;
	for (const auto& pages: s_Read->pages) if (const void* view = pages->ReadAllocation(source)) return view;
	throw std::logic_error(OutsideInventory("object", source, 0));
}

const std::string* CheckpointNativeStorage::ReadString(const char* source, size_t bytes) {
	if (!s_Read) return nullptr;
	static const std::string empty;
	if (!bytes) return &empty;
	const auto key = std::pair{reinterpret_cast<uintptr_t>(source), bytes};
	if (const auto found = s_Read->strings.find(key); found != s_Read->strings.end()) return &found->second;
	CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
	std::string value(bytes, '\0');
	if (!ReadBytes(source, value.data(), bytes)) throw std::logic_error("native string is outside the frozen inventory");
	return &s_Read->strings.emplace(key, std::move(value)).first->second;
}

std::string CheckpointNativeStorage::SelfTestMismatch() {
	{
		std::array<std::byte, 128> source{}, first{}, second{};
		const std::array roots{
			RootRange{source.data() + 16, first.data() + 16, 8},
			RootRange{source.data() + 8, first.data() + 72, 32},
			RootRange{source.data() + 16, second.data() + 16, 8},
			RootRange{source.data() + 48, first.data() + 18, 4, true},
			RootRange{source.data() + 96, second.data() + 96, 0},
			RootRange{source.data() + 64, second.data() + 64, 8, true}
		};
		ReadScope read({}, roots);
		if (RootView(source.data() + 18) != first.data() + 18 || RootView(source.data() + 18, 12) != first.data() + 82 ||
		    RootView(source.data() + 24, 0) != first.data() + 24 || RootView(source.data() + 24) != first.data() + 88 ||
		    RootView(source.data() + 40, 0) != first.data() + 104 || RootView(source.data() + 40) ||
		    RootView(source.data() + 96, 0) != second.data() + 96 || RootView(source.data() + 96))
			return "frozen root lookup changed overlapping owners or byte bounds";
		if (Original(first.data() + 18) != source.data() + 18 || Original(first.data() + 72) != source.data() + 8 ||
		    Original(second.data() + 18) != source.data() + 18 || Original(second.data() + 96) != second.data() + 96 ||
		    IsView(first.data() + 18) || !IsView(first.data() + 22) || !Private(first.data() + 22, 0) ||
		    Private(first.data() + 22) || !PrivateBorrowed(first.data() + 21) || PrivateBorrowed(first.data() + 22))
			return "frozen root lookup changed private values or original identities";
		for (size_t version = 0; version < 130; ++version) {
			auto changed = roots;
			const bool narrow = version % 2 != 0;
			changed[0].bytes = narrow ? 1 : 8;
			changed[3].privateValues = !narrow;
			changed[4].bytes = narrow ? 1 : 0;
			{
				ReadScope replacement({}, changed);
				for (int repeat = 0; repeat < 2; ++repeat) {
					if (RootView(source.data() + 18) != first.data() + (narrow ? 82 : 18) ||
					    RootView(source.data() + 18, 12) != first.data() + 82 ||
					    RootView(source.data() + 96) != (narrow ? second.data() + 96 : nullptr) ||
					    RootView(source.data() + 96, 0) != second.data() + 96 ||
					    PrivateBorrowed(first.data() + 21) == narrow || Private(first.data() + 22))
						return "replacement frozen root index reused another snapshot's answer";
				}
			}
			if (RootView(source.data() + 18) != first.data() + 18 || RootView(source.data() + 96) ||
			    !PrivateBorrowed(first.data() + 21))
				return "closed frozen root index changed the previous reader's answer";
		}
		for (size_t after = 0; after < 3; ++after) {
			CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects, after);
			bool refused = false;
			try { ReadScope refusedRead({}, roots); }
			catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			if (!refused || !Reading() || RootView(source.data() + 18) != first.data() + 18 ||
			    Original(second.data() + 18) != source.data() + 18 || !Private(first.data() + 21))
				return "refused frozen root index changed the previous reader";
		}
		ReadScope retry({}, roots);
		if (RootView(source.data() + 18, 12) != first.data() + 82 || !PrivateBorrowed(second.data() + 64))
			return "frozen root index retry lost its owners";
	}
	{
		CheckpointNativeAllocator<uint64_t> allocator;
		uint64_t* values = nullptr;
		constexpr size_t count = 2048;
		{
			ReadScope read(ReadViews{true, {}, {}});
			const size_t before = PrivateAllocationCount();
			bool refused = false;
			{
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
				try { values = allocator.allocate(count); }
				catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			}
			if (!refused || values || PrivateAllocationCount() != before) return "refused private array retained storage";
			values = allocator.allocate(count);
			if (!Private(values, count * sizeof(uint64_t))) return "large private array lost its saved owner";
			values[0] = 37; values[count - 1] = 71;
			if (values[0] != 37 || values[count - 1] != 71) return "large private array changed its values";
			allocator.deallocate(values, count); values = nullptr;
			if (PrivateAllocationCount() != before) return "large private array kept a released allocation";
			values = allocator.allocate(count);
			values[0] = 43; values[count - 1] = 79;
		}
		if (values[0] != 43 || values[count - 1] != 79) return "large private array lost its values after its reader closed";
		allocator.deallocate(values, count);
	}
	{
		struct Buffer { size_t bytes, alignment; std::unique_ptr<std::byte, decltype(&Deallocate)> owner; };
		std::vector<Buffer> values;
		for (const auto [bytes, alignment]: {std::pair<size_t, size_t>{113, 16}, {203, 32}, {1091, 64}, {3297, 1024}, {12277, 4096}, {131113, 16}}) {
			auto* address = static_cast<std::byte*>(Allocate(bytes, alignment));
			values.push_back({bytes, alignment, {address, &Deallocate}});
			if (reinterpret_cast<uintptr_t>(address) % alignment || !Owns(address + bytes - 1)) return "native slot storage lost its alignment or owner";
			for (size_t byte = 0; byte < bytes; ++byte) address[byte] = static_cast<std::byte>((byte * 37 + values.size()) % 251);
		}
		const auto pages = Prepare();
		for (const auto& page: pages) page->Arm();
		std::vector<const std::byte*> addresses;
		for (auto& value: values) {
			addresses.push_back(value.owner.get());
			std::memset(value.owner.get(), 197, value.bytes);
			value.owner.reset();
			value.owner.reset(static_cast<std::byte*>(Allocate(value.bytes, value.alignment)));
			std::memset(value.owner.get(), 211, value.bytes);
		}
		for (const auto& page: pages) page->Drain();
		ReadScope read(pages);
		for (size_t index = 0; index < values.size(); ++index) {
			std::vector<std::byte> found(values[index].bytes);
			if (!ReadBytes(addresses[index], found.data(), found.size())) return "returned native slots lost their frozen range";
			for (size_t byte = 0; byte < found.size(); ++byte) if (found[byte] != static_cast<std::byte>((byte * 37 + index + 1) % 251))
				return "native slot reuse changed its saved bytes";
		}
	}
	{
		struct RestoreMode { bool previous; ~RestoreMode() { SetWorldEnabled(previous); } } restore{SetWorldEnabled(true)};
		WorldAllocationScope ordinary(false);
		std::unique_ptr<CheckpointVector<uint64_t>> owned;
		{
			WorldAllocationScope multiplayer(true);
			owned = std::make_unique<CheckpointVector<uint64_t>>(4, 17);
		}
		owned->resize(4096, 31);
		if (!Owns(owned->data())) return "owned container growth escapes its original storage";
		CheckpointVector<uint64_t> plain(4, 41), copied(*owned);
		if (Owns(plain.data()) || Owns(copied.data())) return "ordinary container copies enter multiplayer storage";
		const bool workerOwned = std::async(std::launch::async, [] {
			struct Value : CheckpointNativeAllocated {};
			auto value = std::make_unique<Value>();
			return Owns(value.get());
		}).get();
		if (!workerOwned) return "restored world allocations lose their mode on another thread";
		{
			CaptureScope capture;
			auto allocator = owned->get_allocator();
			auto* privateValues = allocator.allocate(4);
			const bool privateOwned = Owns(privateValues);
			allocator.deallocate(privateValues, 4);
			if (privateOwned) return "capture copies allocate in the live world inventory";
		}
	}
	{
		AllocationScope allocation(true);
		auto value = std::make_unique<Vector>(17, 31);
		auto prepared = std::async(std::launch::async, [] { return PrepareInventory(); }).get();
		struct Returned {
			std::vector<void*> addresses;
			~Returned() { for (void* address: addresses) Deallocate(address); }
		} returned;
		size_t available;
		{
			auto& pools = NativePools();
			std::lock_guard lock(pools.mutex);
			available = pools.sizes[20].free.size();
		}
		returned.addresses.reserve(available + 1);
		for (size_t index = 0; index <= available; ++index) returned.addresses.push_back(Allocate(size_t{1} << 20, alignof(std::max_align_t)));
		if (prepared->Current() || prepared->Arm()) return "stale native inventory accepted a new allocation range";
		for (const auto& pages: prepared->Pages()) if (pages->CanBorrow(value.get(), sizeof(Vector)))
			return "stale native inventory fenced a partial boundary";
		prepared.reset();
		prepared = std::async(std::launch::async, [] { return PrepareInventory(); }).get();
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::NativePages);
			if (!prepared->Arm()) return "prepared native inventory refused unchanged ranges";
		}
		value->m_X = 97;
		ReadScope read(prepared->Pages());
		if (Source(value.get())->m_X != 17 || Source(value.get())->m_Y != 31)
			return "prepared native inventory did not preserve its boundary values";
	}
	{
		struct RootValues : Singleton<RootValues> { int tick = 0; CheckpointVector<CheckpointString> names; };
		RootValues::Construct();
		struct Release { ~Release() { RootValues::Destruct(); } } release;
		CheckpointNativeStorage::AllocationScope allocation(true);
		auto& live = RootValues::Instance();
		live.tick = 29; live.names.emplace_back(std::string(337, 'a'));
		Root<RootValues> prepared(&live);
		const auto roots = std::array{prepared.Range()};
		const auto pages = Prepare();
		for (const auto& part: pages) part->Arm();
		prepared.Freeze();
		live.tick = 31; live.names.assign(3, CheckpointString("later"));
		{
			ReadScope read(pages, roots);
			const auto exact = [&] {
				const auto& frozen = RootValues::Instance();
				const auto names = CheckpointValues(frozen.names);
				return frozen.tick == 29 && names.size() == 1 && names.front() == std::string(337, 'a') && Original(&frozen.names) == &live.names;
			};
			if (!exact()) return "frozen manager header lost its values or container identity";
			const ReadViews views = CurrentViews();
			const bool worker = std::async(std::launch::async, [views, &exact] {
				if (Reading() || RootValues::Instance().tick != 31) return false;
				ReadScope read(views);
				return exact();
			}).get();
			if (!worker) return "frozen root scope changed another thread or lost its worker view";
		}
		if (&RootValues::Instance() != &live || live.tick != 31) return "frozen root scope changed its live singleton";
	}
	{
		struct Values : CheckpointNativeAllocated { CheckpointVector<int> numbers; };
		AllocationScope allocation(true);
		auto live = std::make_unique<Values>();
		live->numbers.assign({17, 31});
		const auto* identity = live.get();
		auto prepared = PrepareInventory();
		if (!prepared->Arm()) return "private value preparation refused";
		live->numbers.assign({43});
		for (const auto& page: prepared->Pages()) page->Drain();
		CaptureScope capture;
		Values owned;
		{
			ReadScope read(prepared->Pages());
			for (int number: CheckpointValues(Source(identity)->numbers)) owned.numbers.push_back(number);
		}
		live.reset();
		const std::array roots{RootRange{identity, &owned, sizeof(owned), true}};
		ReadScope read(prepared->Pages(), roots);
		if (Source(identity) != &owned || Original(&owned) != identity || IsView(&owned.numbers)) return "private value lost its original identity";
		const auto numbers = CheckpointValues(owned.numbers);
		if (numbers.size() != 2 || numbers.front() != 17 || numbers.back() != 31) return "private value containers read source nodes";
	}
	{
		constexpr size_t bytes = c_NativeStorageBlockBytes + 17;
		const auto release = [](unsigned char* address) { Deallocate(address); };
		auto storage = std::unique_ptr<unsigned char, decltype(release)>(static_cast<unsigned char*>(Allocate(bytes, alignof(std::max_align_t))), release);
		storage.get()[0] = 37; storage.get()[bytes - 1] = 71;
		const auto* identity = storage.get();
		{
			auto& pools = NativePools();
			std::lock_guard lock(pools.mutex);
			const auto found = pools.large.find(reinterpret_cast<uintptr_t>(identity));
			const size_t page = PageWriteFence::SystemPageBytes();
			if (found == pools.large.end() || found->second->Bytes() != (bytes + page - 1) / page * page)
				return "large native buffer retained unused power-of-two storage";
		}
		auto prepared = PrepareInventory();
		if (!prepared->Arm()) return "large native buffer preparation refused";
		storage.get()[0] = 97;
		storage.reset();
		if (Owns(identity) || prepared->Current()) return "freed large native buffer remains in the live inventory";
		for (const auto& pages: prepared->Pages()) pages->Drain();
		{
			ReadScope read(prepared->Pages());
			if (*Source(identity) != 37 || *Source(identity + bytes - 1) != 71)
				return "retired large native buffer changed its saved pages";
		}
		const auto generation = CheckpointPagePool::InventoryGeneration();
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::NativePages);
			bool refused = false;
			try { storage.reset(static_cast<unsigned char*>(Allocate(bytes, alignof(std::max_align_t)))); }
			catch (const std::bad_alloc&) { refused = true; }
			if (!refused || generation != CheckpointPagePool::InventoryGeneration()) return "refused large native allocation published a range";
		}
		storage.reset(static_cast<unsigned char*>(Allocate(bytes, alignof(std::max_align_t))));
		if (!Owns(storage.get())) return "large native buffer retry lost its owner";
	}
	std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> first, second;
	const void* address = nullptr;
	const std::vector<uint64_t> expected{3, 5, 7, 11};
	{
		AllocationScope allocation(true);
		std::vector<uint64_t, CheckpointNativeAllocator<uint64_t>> live(expected.begin(), expected.end());
		address = live.data();
		if (!Owns(address)) return "native container storage escaped its owned pages";
		first = Prepare();
		for (const auto& pages: first) pages->Arm();
		live[1] = 17;
		second = Prepare();
		for (const auto& pages: second) pages->Arm();
		live.clear(); live.shrink_to_fit();
		std::vector<uint64_t, CheckpointNativeAllocator<uint64_t>> reused(4, 99);
	}
	for (const auto& pages: first) pages->Drain();
	for (const auto& pages: second) pages->Drain();
	std::vector<uint64_t> read(expected.size());
	bool foundFirst = false, foundSecond = false;
	for (const auto& pages: first) if (pages->Read(address, read.data(), read.size() * sizeof(uint64_t))) {
		foundFirst = true;
		if (read != expected) return "native container mutation or destruction changed its first generation";
	}
	for (const auto& pages: second) if (pages->Read(address, read.data(), read.size() * sizeof(uint64_t))) {
		foundSecond = true;
		if (read != std::vector<uint64_t>{3, 17, 7, 11}) return "native container reuse changed its second generation";
	}
	if (!foundFirst || !foundSecond) return "native container generation lost its allocation";
	{
		CheckpointNativeStorage::AllocationScope allocation(true);
		struct First { virtual ~First() = default; };
		struct Second { virtual ~Second() = default; };
		struct Whole : First, Second, CheckpointNativeAllocated { std::array<uint64_t, 700> fields{}; };
		auto live = std::make_unique<Whole>();
		live->fields.front() = 71; live->fields.back() = 139;
		const Second* address = live.get();
		const auto pages = Prepare();
		for (const auto& part: pages) part->Arm();
		live.reset();
		auto reused = std::make_unique<Whole>();
		CheckpointNativeStorage::ReadScope read(pages);
		const auto* whole = dynamic_cast<const Whole*>(Source(address));
		if (!whole || whole->fields.front() != 71 || whole->fields.back() != 139) return "frozen secondary base lost its complete allocation";
	}
	{
		CheckpointNativeStorage::AllocationScope allocation(true);
		struct alignas(64) AlignedValue : Vector {};
		auto value = std::make_unique<AlignedValue>();
		auto shared = MakeCheckpointNativeShared<Vector>(3.0F, 7.0F);
		auto array = std::make_unique<Vector[]>(3);
		value->m_X = 13; array[2].m_Y = 19;
		const float* fields[] = {&value->m_X, &shared->m_Y, &array[2].m_Y};
		if (reinterpret_cast<uintptr_t>(value.get()) % alignof(AlignedValue)) return "native value alignment changed";
		const auto pages = Prepare();
		for (const auto& part: pages) part->Arm();
		value.reset(); shared.reset(); array.reset();
		const float expected[] = {13, 7, 19};
		for (size_t index = 0; index < std::size(fields); ++index) {
			float read = 0;
			bool found = false;
			for (const auto& part: pages) if (part->Read(fields[index], &read, sizeof(read))) { found = true; break; }
			if (!found || read != expected[index]) return "destroyed native value lost its frozen fields";
		}
	}
	{
		ReadScope read(ReadViews{true, {}, {}});
		struct alignas(64) Values : CheckpointNativeAllocated { CheckpointVector<int> numbers; };
		auto value = std::make_unique<Values>();
		value->numbers.assign({17, 31});
		if (Owns(value.get()) || Owns(value->numbers.data()) || !Private(value.get(), sizeof(Values)))
			return "a saved getter put its new values in live storage";
		{
			ReadScope nested(ReadViews{true, {}, {}});
			if (Source(value.get()) != value.get() || CheckpointValues(Source(value.get())->numbers).back() != 31)
				return "a nested saved getter lost its private values";
		}
		Values unknown;
		bool refused = false;
		try { Source(&unknown); } catch (const std::logic_error&) { refused = true; }
		if (!refused) return "saved getters accepted unrecorded native storage";
		try {
			CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
			auto failed = std::make_unique<Values>();
			return "a saved getter ignored its allocation refusal";
		} catch (const std::bad_alloc&) {}
		auto retry = std::make_unique<Values>();
		retry->numbers.assign({43});
		if (Source(retry.get())->numbers.front() != 43 || Source(value.get())->numbers.back() != 31)
			return "a saved getter refusal changed its earlier private values";
		const auto views = CurrentViews();
		const bool workerRead = std::async(std::launch::async, [views, target = value.get()] {
			ReadScope read(views);
			if (Source(target) != target || Source(target)->numbers.back() != 31) return false;
			int number = 0;
			return ReadBytes(target->numbers.data(), &number, sizeof(number)) && number == 17;
		}).get();
		if (!workerRead) return "a saved worker lost its capture's private native values";
	}
	{
		ReadViews views;
		std::unique_ptr<Vector> value;
		{
			ReadScope read(ReadViews{true, {}, {}});
			value = std::make_unique<Vector>(17, 31);
			views = CurrentViews();
		}
		const bool readLater = std::async(std::launch::async, [views, target = value.get()] {
			ReadScope read(views);
			return Source(target) == target && Source(target)->m_X == 17 && Source(target)->m_Y == 31;
		}).get();
		ReadScope read(views);
		value.reset();
		if (!readLater || PrivateAllocationCount() != 0) return "saved read values did not retain and release their private storage";
	}
	{
		ReadViews child;
		std::weak_ptr<const void> parent;
		{
			ReadScope first(ReadViews{true, {}, {}});
			const auto views = CurrentViews();
			parent = views.owner;
			ReadScope second(views);
			child = CurrentViews();
		}
		if (parent.expired()) return "a saved child reader lost its parent's stored values";
		child = {};
		if (!parent.expired()) return "a finished saved child reader retained its parent";
	}
	{
		// Consecutive inventory boundaries: a page stays fenced between captures, a written page is copied at the next boundary,
		// and a write after a boundary never reaches that boundary's copy.
		const auto freed = [](uint64_t* value) { Deallocate(value); };
		std::unique_ptr<uint64_t, decltype(freed)> cold(static_cast<uint64_t*>(Allocate(8192, 4096)), freed), hot(static_cast<uint64_t*>(Allocate(8192, 4096)), freed);
		cold.get()[0] = 11; hot.get()[0] = 21;
		const auto read = [](std::span<const std::shared_ptr<const CheckpointPagePool::Snapshot>> pages, const uint64_t* source) {
			uint64_t value = 0;
			ReadScope scope(pages);
			if (!ReadBytes(source, &value, sizeof(value))) throw std::runtime_error("an inventory value was outside its pages");
			return value;
		};
		{
			const auto first = Prepare();
			for (const auto& page: first) page->Arm();
			cold.get()[0] = 12; hot.get()[0] = 22;
			if (read(first, cold.get()) != 11 || read(first, hot.get()) != 21) return "a first inventory boundary lost its fenced values";
		}
		// Written pages that stop changing are fenced again and still give the next boundary their bytes.
		CheckpointPagePool::CoolInventory();
		if (CheckpointPagePool::CoolInventory() == 0) return "written pages that stopped changing were never fenced again";
		{
			const auto second = Prepare();
			for (const auto& page: second) page->WarmShadow();
			for (const auto& page: second) page->Arm();
			hot.get()[0] = 23;
			if (read(second, cold.get()) != 12 || read(second, hot.get()) != 22) return "a written page lost the value of the next boundary";
		}
		{
			const auto third = Prepare();
			for (const auto& page: third) page->Arm();
			cold.get()[0] = 13;
			if (read(third, cold.get()) != 12 || read(third, hot.get()) != 23) return "a page fenced again after its capture lost its value";
		}
		if (CheckpointPagePool::SettleInventory(SIZE_MAX) != 0) return "settling left unseen inventory pages open";
		cold.get()[0] = 14; hot.get()[0] = 24;
		const auto settled = Prepare();
		for (const auto& page: settled) page->Arm();
		cold.get()[0] = 15; hot.get()[0] = 25;
		if (read(settled, cold.get()) != 14 || read(settled, hot.get()) != 24) return "settled pages lost the values of their boundary";
	}
	if (const auto mismatch = CheckpointString::SelfTestMismatch(); !mismatch.empty()) return mismatch;
	return CheckpointFrozenContainersSelfTestMismatch();
}

namespace {
	template<class T> concept FrozenPairValue = requires(const T& value) { value.first; value.second; };
	template<class Container> concept KeyedContainer = requires { typename Container::key_type; };
	template<class Container> concept HashedContainer = requires(const Container& container) { container.hash_function(); };
	template<class Container> concept PushBackContainer = requires(Container& container, CheckpointString value) { container.push_back(std::move(value)); };
	template<class Container> concept PushContainer = requires(Container& container, CheckpointString value) { container.push(std::move(value)); };

	template<class T> std::string RenderFrozenValue(const T& value) {
		if constexpr (FrozenPairValue<T>) return std::to_string(value.first) + ":" + value.second.Value();
		else return value.Value();
	}

	template<class Key> Key MissingFrozenKey() {
		if constexpr (std::is_integral_v<Key>) return Key{-1};
		else return Key("missing checkpoint lookup key");
	}

	// Every container kind is filled, frozen, destroyed and read back the same way.
	template<class Container> std::string FrozenContainerMismatch(const char* name) {
		CheckpointNativeStorage::AllocationScope allocation(true);
		auto live = MakeCheckpointNativeShared<Container>();
		for (int index = 0; index < 133; ++index) {
			CheckpointString value(std::to_string((index * 73) % 133) + std::string(19, 'a' + index % 26));
			if constexpr (CheckpointContainerDetail::MappedContainer<Container>) live->emplace(index, std::move(value));
			else if constexpr (PushBackContainer<Container>) live->push_back(std::move(value));
			else if constexpr (PushContainer<Container>) live->push(std::move(value));
			else live->insert(std::move(value));
		}
		std::vector<std::string> expected;
		for (const auto& value: CheckpointValues(*live)) expected.push_back(RenderFrozenValue(value));
		const Container* address = live.get();
		const auto pages = CheckpointNativeStorage::Prepare();
		for (const auto& part: pages) part->Arm();
		live.reset();
		auto reused = MakeCheckpointNativeShared<Container>();
		CheckpointNativeStorage::ReadScope read(pages);
		const auto& frozen = *static_cast<const Container*>(CheckpointNativeStorage::View(address, sizeof(Container)));
		if (CheckpointNativeStorage::Original(&frozen) != address) return std::string(name) + " lost its original address";
		std::vector<std::string> found;
		for (const auto& value: CheckpointValues(frozen)) {
			found.push_back(RenderFrozenValue(value));
			if constexpr (KeyedContainer<Container>) {
				const auto& key = CheckpointContainerDetail::LookupKey<Container>(value);
				for (int repeat = 0; repeat < 2; ++repeat) if (CheckpointFind(frozen, key) != &value) return std::string(name) + " lookup escaped its frozen generation";
			}
		}
		if constexpr (KeyedContainer<Container>) {
			using Key = typename Container::key_type;
			const Key missing = MissingFrozenKey<Key>();
			if (CheckpointContains(frozen, missing)) return std::string(name) + " frozen index invented a value";
		}
		if constexpr (HashedContainer<Container>) {
			const auto rebuilt = CheckpointRebuildHash(frozen, [](const auto& value) { return value; });
			std::vector<std::string> copied;
			for (const auto& value: rebuilt) copied.push_back(RenderFrozenValue(value));
			if (copied != expected || rebuilt.bucket_count() != frozen.bucket_count()) return std::string(name) + " changed order or buckets when rebuilt";
		}
		return found == expected ? std::string() : std::string(name) + " changed values or order after destruction";
	}
}

std::string RTE::CheckpointFrozenContainersSelfTestMismatch() {
	{
		CheckpointNativeStorage::AllocationScope allocation(true);
		struct Values {
			CheckpointVector<bool> flags;
			CheckpointMap<int, CheckpointList<CheckpointString>> nested;
			CheckpointUnorderedMap<int, CheckpointString> hash;
		};
		auto live = MakeCheckpointNativeShared<Values>();
		for (int index = 0; index < 8197; ++index) live->flags.push_back((index % 3 == 0) != (index % 71 == 0));
		for (int index = 0; index < 17; ++index) {
			live->nested[index].push_back(std::string(117 + index, 'a' + index));
			live->nested[index].push_back(std::to_string(index));
			live->hash.emplace(index, std::string(151 + index, 'z' - index));
		}
		const auto save = [](const Values& values) {
			CheckpointWriter writer("FrozenContainers1");
			writer(values.flags, values.nested, values.hash);
			return writer.Text();
		};
		const std::string expected = save(*live);
		const Values* address = live.get();
		const auto pages = CheckpointNativeStorage::Prepare();
		for (const auto& part: pages) part->Arm();
		live.reset();
		auto reused = MakeCheckpointNativeShared<Values>();
		reused->flags.assign(8197, true);
		CheckpointNativeStorage::ReadScope read(pages);
		const auto* frozen = CheckpointNativeStorage::Source(address);
		if (save(*frozen) != expected) return "frozen nested or packed containers changed their archive bytes";
		for (bool batched: {false, true}) {
			CheckpointWriter::BatchOverride batch(batched);
			if (CheckpointWriter::CaptureNative([&] { return save(*frozen); }).Text() != expected) return "frozen container capture changed its archive bytes";
		}
	}
	struct Collisions {
		size_t operator()(int key) const { return static_cast<size_t>(key % 7); }
		size_t operator()(const CheckpointString& key) const { return std::hash<CheckpointString>{}(key) % 7; }
	};
	std::string mismatch;
	if (!(mismatch = FrozenContainerMismatch<CheckpointVector<CheckpointString>>("vector")).empty()) return mismatch;
	if (!(mismatch = FrozenContainerMismatch<CheckpointDeque<CheckpointString>>("deque")).empty()) return mismatch;
	if (!(mismatch = FrozenContainerMismatch<CheckpointList<CheckpointString>>("list")).empty()) return mismatch;
	if (!(mismatch = FrozenContainerMismatch<CheckpointQueue<CheckpointString>>("queue")).empty()) return mismatch;
	if (!(mismatch = FrozenContainerMismatch<CheckpointSet<CheckpointString, std::greater<CheckpointString>>>("set")).empty()) return mismatch;
	if (!(mismatch = FrozenContainerMismatch<CheckpointMap<int, CheckpointString>>("map")).empty()) return mismatch;
	if (!(mismatch = FrozenContainerMismatch<CheckpointUnorderedMap<int, CheckpointString, Collisions>>("hash map")).empty()) return mismatch;
	return FrozenContainerMismatch<CheckpointUnorderedSet<CheckpointString, Collisions>>("hash set");
}

std::string CheckpointString::SelfTestMismatch() {
	const std::string before = std::string(513, 'a') + std::string("\0saved", 6);
	const std::string after(1025, 'b');
	std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> first, second;
	const CheckpointString* source = nullptr;
	{
		CheckpointNativeStorage::AllocationScope allocation(true);
		auto live = std::allocate_shared<CheckpointString>(CheckpointNativeAllocator<CheckpointString>{}, before);
		{
			CheckpointNativeStorage::CaptureScope ordinary;
			*live = before;
		}
		source = live.get();
		const std::string& alias = *live;
		const char* originalBytes = alias.data();
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
			bool refused = false;
			try { live->PrepareCheckpointStorage(); } catch (const std::bad_alloc&) { refused = failure.Triggered(); }
			if (!refused || alias != before || live->m_Mirrored || alias.data() != originalBytes) return "refused string preparation changed its live value";
		}
		live->PrepareCheckpointStorage();
		if (&alias != &live->Value() || alias.data() != originalBytes) return "string preparation replaced a legacy reference";
		first = CheckpointNativeStorage::Prepare();
		for (const auto& pages: first) pages->Arm();
		*live = after;
		if (alias != after || &alias != &live->Value()) return "native string lost its live reference";
		second = CheckpointNativeStorage::Prepare();
		for (const auto& pages: second) pages->Arm();
		live->clear(); live.reset();
		auto reused = std::allocate_shared<CheckpointString>(CheckpointNativeAllocator<CheckpointString>{}, "reused");
	}
	for (const auto& pages: first) pages->Drain();
	for (const auto& pages: second) pages->Drain();
	const auto check = [&](const auto& pages, const std::string& expected) {
		CheckpointNativeStorage::ReadScope read(pages);
		alignas(CheckpointString) std::array<std::byte, sizeof(CheckpointString)> header;
		if (!CheckpointNativeStorage::ReadBytes(source, header.data(), header.size())) return false;
		const auto& frozen = *reinterpret_cast<const CheckpointString*>(header.data());
		return frozen.Value() == expected && std::hash<CheckpointString>{}(frozen) == std::hash<std::string>{}(expected);
	};
	if (!check(first, before) || !check(second, after)) return "native string edit or destruction changed its frozen bytes";
	return {};
}
