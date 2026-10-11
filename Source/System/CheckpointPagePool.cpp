#include "CheckpointPagePool.h"

#include "PageWriteFence.h"
#include "CheckpointFailure.h"
#include "CheckpointNativeStorage.h"
#include "CheckpointString.h"
#include "CheckpointFrozenContainers.h"
#include "CheckpointArchive.h"
#include "Singleton.h"
#include "ScenarioRunner.h"
#include "Vector.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using namespace RTE;

namespace {
	int64_t Since(std::chrono::steady_clock::time_point start) {
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	}
	struct Pages {
		unsigned char* data = nullptr;
		size_t bytes;
		explicit Pages(size_t bytes) : bytes(bytes) {
			CheckpointFailure::Check(CheckpointFailure::Point::NativePages);
#ifdef _WIN32
			data = static_cast<unsigned char*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
			void* memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (memory != MAP_FAILED) data = static_cast<unsigned char*>(memory);
#endif
			if (!data) throw std::bad_alloc();
		}
		~Pages() {
#ifdef _WIN32
			VirtualFree(data, 0, MEM_RELEASE);
#else
			munmap(data, bytes);
#endif
		}
	};
	struct Locked {
		std::atomic_flag& flag;
		explicit Locked(std::atomic_flag& flag) : flag(flag) { while (flag.test_and_set(std::memory_order_acquire)) {} }
		~Locked() { flag.clear(std::memory_order_release); }
	};
}

struct CheckpointPagePool::Copy {
	Pages pages;
	std::vector<std::atomic<unsigned char>> saved;
	std::thread::id simThread;
	std::atomic<bool> armed{false};
	std::atomic<int64_t> workerUs{0}, simFaultNs{0}, otherFaultNs{0};
	std::atomic<uint64_t> simFaults{0}, otherFaults{0};
	bool completed = false;
	bool abandoned = false;
	Copy(size_t bytes, size_t pageBytes) : pages(bytes), saved(bytes / pageBytes) {
		for (auto& page: saved) page.store(0, std::memory_order_relaxed);
	}
};

struct CheckpointPagePool::Block {
	Pages live;
	size_t slotBytes;
	size_t pageBytes = PageWriteFence::SystemPageBytes();
	std::atomic_flag lock = ATOMIC_FLAG_INIT;
	std::vector<std::shared_ptr<Copy>> copies;
	explicit Block(size_t bytes, size_t slotBytes = 0) : live(bytes), slotBytes(slotBytes ? slotBytes : bytes) {}
	~Block() { PageWriteFence::UnwatchCopies(this); }
	void Prepare(const std::shared_ptr<Copy>& copy) {
		const auto keepWritable = [](void*, uintptr_t, size_t) noexcept { return true; };
		if (!PageWriteFence::WatchCopies(this, {live.data, live.bytes}, OnWrite, this, keepWritable))
			throw std::runtime_error("could not prepare native checkpoint page watches");
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
		copy->armed.store(true, std::memory_order_release);
	}
	bool SavePages(size_t first, size_t count, bool fault) noexcept {
		const auto started = fault ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		Locked guard(lock);
		bool savedAny = false;
		for (const auto& copy: copies) {
			if (!copy->armed.load(std::memory_order_relaxed)) continue;
			for (size_t page = first; page < first + count; ++page) {
				if (copy->saved[page].load(std::memory_order_relaxed)) continue;
				const size_t offset = page * pageBytes;
				std::memcpy(copy->pages.data + offset, live.data + offset, pageBytes);
				copy->saved[page].store(1, std::memory_order_release);
				savedAny = true;
			}
		}
		const bool opened = (!fault && !savedAny) || PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(live.data + first * pageBytes), count * pageBytes);
		if (fault) {
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
			for (const auto& copy: copies) if (copy->armed.load(std::memory_order_relaxed) && !copy->completed) {
				const bool simulation = std::this_thread::get_id() == copy->simThread;
				(simulation ? copy->simFaults : copy->otherFaults).fetch_add(1, std::memory_order_relaxed);
				(simulation ? copy->simFaultNs : copy->otherFaultNs).fetch_add(elapsed, std::memory_order_relaxed);
			}
		}
		return opened;
	}
	bool SavePage(size_t page, bool fault) noexcept { return SavePages(page, 1, fault); }
	static bool OnWrite(void* context, uintptr_t address) noexcept {
		auto& block = *static_cast<Block*>(context);
		return block.SavePage((address - reinterpret_cast<uintptr_t>(block.live.data)) / block.pageBytes, true);
	}
	void Complete(const std::shared_ptr<Copy>& copy) {
		Locked guard(lock);
		copy->completed = true;
		if (std::all_of(copies.begin(), copies.end(), [](const auto& value) { return !value->armed.load(std::memory_order_relaxed) || value->completed; }) &&
		    !PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(live.data), live.bytes))
			throw std::runtime_error("could not open copied native checkpoint pages");
	}
	void Abandon(const std::shared_ptr<Copy>& copy) noexcept {
		Locked guard(lock);
		std::erase(copies, copy);
		copy->abandoned = true;
		copy->armed.store(false, std::memory_order_release);
		if (std::all_of(copies.begin(), copies.end(), [](const auto& value) { return !value->armed.load(std::memory_order_relaxed) || value->completed; }))
			PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(live.data), live.bytes);
	}
	bool Contains(const void* source, size_t bytes) const {
		const uintptr_t from = reinterpret_cast<uintptr_t>(source), base = reinterpret_cast<uintptr_t>(live.data);
		return from >= base && from - base <= live.bytes && bytes <= live.bytes - (from - base);
	}
};

void CheckpointPagePool::Grow(size_t slotBytes, size_t minimumSlots, std::vector<void*>& free, size_t blockBytes) {
	const size_t pageBytes = PageWriteFence::SystemPageBytes();
	if (!slotBytes || !pageBytes || minimumSlots > (std::numeric_limits<size_t>::max() - pageBytes) / slotBytes)
		throw std::length_error("native checkpoint pool is too large");
	const size_t slots = std::max(minimumSlots, blockBytes / slotBytes);
	if (slots > std::numeric_limits<size_t>::max() - m_Slots) throw std::bad_alloc();
	const size_t bytes = (slots * slotBytes + pageBytes - 1) / pageBytes * pageBytes;
	auto block = std::make_shared<Block>(bytes, slotBytes);
	free.reserve(m_Slots + slots);
	m_Blocks.push_back(block);
	for (size_t slot = 0; slot < slots; ++slot) free.push_back(block->live.data + slot * slotBytes);
	m_Slots += slots;
}

bool CheckpointPagePool::Contains(const void* address) const {
	return std::any_of(m_Blocks.begin(), m_Blocks.end(), [&](const auto& block) { return block->Contains(address, 1); });
}

CheckpointPagePool::Allocation::Allocation(size_t bytes) {
	const size_t page = PageWriteFence::SystemPageBytes();
	if (!page || bytes > std::numeric_limits<size_t>::max() - page) throw std::bad_alloc();
	m_Block = std::make_shared<Block>(std::max(page, (bytes + page - 1) / page * page));
}
void* CheckpointPagePool::Allocation::Data() const { return m_Block->live.data; }
size_t CheckpointPagePool::Allocation::Bytes() const { return m_Block->live.bytes; }
bool CheckpointPagePool::Allocation::Contains(const void* source, size_t bytes) const { return m_Block->Contains(source, bytes); }
std::shared_ptr<const CheckpointPagePool::Snapshot> CheckpointPagePool::Allocation::Freeze(bool arm) const {
	auto snapshot = std::make_shared<Snapshot>();
	snapshot->m_Parts.reserve(1);
	auto copy = std::make_shared<Copy>(m_Block->live.bytes, m_Block->pageBytes);
	snapshot->m_Parts.push_back({m_Block, std::move(copy)});
	m_Block->Prepare(snapshot->m_Parts.back().copy);
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
	if (!part.copy->armed.load(std::memory_order_acquire)) throw std::logic_error("native checkpoint read before its boundary");
	const size_t offset = address - reinterpret_cast<uintptr_t>(part.block->live.data);
	if (bytes) {
		const size_t first = offset / part.block->pageBytes, last = (offset + bytes - 1) / part.block->pageBytes;
		for (size_t page = first; page <= last; ++page) {
			if (!part.copy->saved[page].load(std::memory_order_acquire) && !part.block->SavePage(page, false))
				throw std::runtime_error("could not read a frozen native page");
		}
	}
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
	std::sort(snapshot->m_Parts.begin(), snapshot->m_Parts.end(), [](const auto& a, const auto& b) {
		return reinterpret_cast<uintptr_t>(a.block->live.data) < reinterpret_cast<uintptr_t>(b.block->live.data);
	});
	if (arm) snapshot->Arm();
	return snapshot;
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
	for (const auto& part: m_Parts) {
		const uintptr_t first = reinterpret_cast<uintptr_t>(part.copy->pages.data);
		if (address >= first && address - first <= part.copy->pages.bytes && bytes <= part.copy->pages.bytes - (address - first))
			return part.block->live.data + (address - first);
	}
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
	if (!part.copy->armed.load(std::memory_order_acquire)) return false;
	if (!bytes) return true;
	const size_t first = (address - reinterpret_cast<uintptr_t>(part.block->live.data)) / part.block->pageBytes;
	const size_t last = (address - reinterpret_cast<uintptr_t>(part.block->live.data) + bytes - 1) / part.block->pageBytes;
	for (size_t page = first; page <= last; ++page) if (part.copy->saved[page].load(std::memory_order_acquire)) return false;
	return true;
}

CheckpointPagePool::Snapshot::~Snapshot() {
	for (const auto& part: m_Parts) part.block->Abandon(part.copy);
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
	try {
		for (const auto& part: m_Parts) part.block->Arm(part.copy);
	} catch (...) {
		for (const auto& part: m_Parts) part.block->Abandon(part.copy);
		throw;
	}
}

void CheckpointPagePool::Snapshot::Drain() const {
	for (const auto& part: m_Parts) {
		if (!part.copy->armed.load(std::memory_order_acquire)) throw std::logic_error("native checkpoint drain before its boundary");
		const auto start = std::chrono::steady_clock::now();
		const size_t batch = std::max<size_t>(1, (size_t{64} << 10) / part.block->pageBytes);
		for (size_t page = 0; page < part.copy->saved.size(); page += batch)
			if (!part.block->SavePages(page, std::min(batch, part.copy->saved.size() - page), false)) throw std::runtime_error("could not drain native checkpoint pages");
		part.block->Complete(part.copy);
		part.copy->workerUs.fetch_add(Since(start), std::memory_order_relaxed);
	}
}

CheckpointPagePool::Costs CheckpointPagePool::Snapshot::Cost() const {
	Costs result;
	for (const auto& part: m_Parts) {
		result.bytes += part.copy->pages.bytes;
		result.workerUs += part.copy->workerUs.load(std::memory_order_relaxed);
		result.simFaultUs += (part.copy->simFaultNs.load(std::memory_order_relaxed) + 999) / 1000;
		result.otherFaultUs += (part.copy->otherFaultNs.load(std::memory_order_relaxed) + 999) / 1000;
		result.simFaults += part.copy->simFaults.load(std::memory_order_relaxed);
		result.otherFaults += part.copy->otherFaults.load(std::memory_order_relaxed);
	}
	return result;
}

std::string CheckpointPagePool::SelfTestMismatch() {
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
	struct NativeStoragePool {
		CheckpointPagePool pages;
		std::vector<void*> free;
	};
	struct NativeStoragePools {
		std::mutex mutex;
		std::array<NativeStoragePool, std::numeric_limits<size_t>::digits> sizes;
	};
	std::atomic<NativeStoragePools*> s_NativeStoragePools{nullptr};
	thread_local bool s_NativeStorageAllocation = false;
	thread_local bool s_NativeStorageCapture = false;
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

bool CheckpointNativeStorage::Enabled() { return !s_NativeStorageCapture && (s_NativeStorageAllocation || ScenarioRunner::HasLockstepCoordinator()); }

void* CheckpointNativeStorage::Allocate(size_t bytes, size_t alignment) {
	const size_t requested = std::max({bytes, alignment, size_t{16}});
	if (requested > (size_t{1} << (std::numeric_limits<size_t>::digits - 1)) ||
	    !std::has_single_bit(alignment) || alignment > PageWriteFence::SystemPageBytes()) throw std::bad_alloc();
	const size_t slot = std::bit_ceil(requested);
	auto& pools = NativePools();
	std::lock_guard lock(pools.mutex);
	auto& pool = pools.sizes[std::countr_zero(slot)];
	if (pool.free.empty()) pool.pages.Grow(slot, 1, pool.free, size_t{1} << 20);
	void* address = pool.free.back();
	pool.free.pop_back();
	return address;
}

bool CheckpointNativeStorage::Deallocate(void* address) noexcept {
	if (!address) return false;
	auto* pools = s_NativeStoragePools.load(std::memory_order_acquire);
	if (!pools) return false;
	std::lock_guard lock(pools->mutex);
	for (auto& pool: pools->sizes) if (pool.pages.Contains(address)) {
		// Free lists reserve every slot at growth, so returning storage cannot allocate or discard frozen pages.
		pool.free.push_back(address);
		return true;
	}
	return false;
}

bool CheckpointNativeStorage::Owns(const void* address) {
	auto* pools = s_NativeStoragePools.load(std::memory_order_acquire);
	if (!pools) return false;
	std::lock_guard lock(pools->mutex);
	return std::any_of(pools->sizes.begin(), pools->sizes.end(), [&](const auto& pool) { return pool.pages.Contains(address); });
}

std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> CheckpointNativeStorage::Prepare() {
	std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> snapshots;
	auto* pools = s_NativeStoragePools.load(std::memory_order_acquire);
	if (!pools) return snapshots;
	std::array<CheckpointPagePool, std::numeric_limits<size_t>::digits> inventory;
	{
		std::lock_guard lock(pools->mutex);
		for (size_t index = 0; index < inventory.size(); ++index) inventory[index] = pools->sizes[index].pages;
	}
	snapshots.reserve(inventory.size());
	for (const auto& pool: inventory) snapshots.push_back(pool.Freeze(false));
	return snapshots;
}

struct CheckpointNativeStorage::ReadState {
	std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> pages;
	std::vector<RootRange> roots;
	std::map<std::pair<uintptr_t, size_t>, std::string> strings;
	std::map<std::pair<uintptr_t, uintptr_t>, std::shared_ptr<const void>> indices;
};

thread_local CheckpointNativeStorage::ReadState* CheckpointNativeStorage::s_Read = nullptr;

CheckpointNativeStorage::ReadScope::ReadScope(std::span<const std::shared_ptr<const CheckpointPagePool::Snapshot>> pages, std::span<const RootRange> roots)
	: ReadScope(ReadViews{true, pages, roots}) {}

CheckpointNativeStorage::ReadScope::ReadScope(ReadViews views) : m_Capture(views.active), m_Previous(s_Read) {
	if (!views.active) return;
	m_State = std::make_unique<ReadState>();
	m_State->pages.assign(views.pages.begin(), views.pages.end());
	m_State->roots.assign(views.roots.begin(), views.roots.end());
	s_Read = m_State.get();
}

CheckpointNativeStorage::ReadViews CheckpointNativeStorage::CurrentViews() {
	return s_Read ? ReadViews{true, s_Read->pages, s_Read->roots} : ReadViews{};
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
	for (const RootRange& root: s_Read->roots) {
		const uintptr_t start = reinterpret_cast<uintptr_t>(root.source);
		if (address >= start && address - start <= root.bytes && bytes <= root.bytes - (address - start))
			return static_cast<const std::byte*>(root.view) + (address - start);
	}
	return nullptr;
}

CheckpointNativeStorage::ReadScope::~ReadScope() { s_Read = m_Previous; }

bool CheckpointNativeStorage::ReadBytes(const void* source, void* target, size_t bytes) {
	source = Original(source);
	if (const void* root = RootView(source, bytes)) { std::memcpy(target, root, bytes); return true; }
	if (s_Read) for (const auto& pages: s_Read->pages) if (pages->Read(source, target, bytes)) return true;
	return false;
}

const void* CheckpointNativeStorage::Original(const void* view) {
	if (s_Read && view) for (const RootRange& root: s_Read->roots) {
		const uintptr_t address = reinterpret_cast<uintptr_t>(view), start = reinterpret_cast<uintptr_t>(root.view);
		if (address >= start && address - start < root.bytes) return static_cast<const std::byte*>(root.source) + (address - start);
	}
	if (s_Read) for (const auto& pages: s_Read->pages) if (const void* source = pages->Original(view)) return source;
	return view;
}

const void* CheckpointNativeStorage::View(const void* source, size_t bytes) {
	if (!s_Read || !source) return source;
	if (const void* root = RootView(source, bytes)) return root;
	for (const auto& pages: s_Read->pages) if (pages->Original(source, bytes)) return source;
	for (const auto& pages: s_Read->pages) if (pages->Contains(source, bytes)) return pages->ReadBytes(source, bytes).data();
	throw std::logic_error("native value is outside the frozen inventory");
}

std::shared_ptr<const CheckpointPagePool::Snapshot> CheckpointNativeStorage::PagesFor(const void* source, size_t bytes) {
	source = Original(source);
	if (s_Read) for (const auto& pages: s_Read->pages) if (pages->Contains(source, bytes)) return pages;
	return {};
}

const void* CheckpointNativeStorage::ViewObject(const void* source) {
	if (!s_Read || !source) return source;
	source = Original(source);
	if (const void* root = RootView(source)) return root;
	for (const auto& pages: s_Read->pages) if (const void* view = pages->ReadAllocation(source)) return view;
	throw std::logic_error("native object is outside the frozen inventory");
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
	if (const auto mismatch = CheckpointString::SelfTestMismatch(); !mismatch.empty()) return mismatch;
	return CheckpointFrozenContainersSelfTestMismatch();
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
	const auto render = []<class T>(const T& value) -> std::string {
		if constexpr (requires { value.first; value.second; }) return std::to_string(value.first) + ":" + value.second.Value();
		else return value.Value();
	};
	const auto check = [&]<class Container>(Container*, const char* name) -> std::string {
		CheckpointNativeStorage::AllocationScope allocation(true);
		auto live = MakeCheckpointNativeShared<Container>();
		for (int index = 0; index < 133; ++index) {
			CheckpointString value(std::to_string((index * 73) % 133) + std::string(19, 'a' + index % 26));
			if constexpr (requires { typename Container::mapped_type; }) live->emplace(index, std::move(value));
			else if constexpr (requires { live->push_back(std::move(value)); }) live->push_back(std::move(value));
			else if constexpr (requires { live->push(std::move(value)); }) live->push(std::move(value));
			else live->insert(std::move(value));
		}
		std::vector<std::string> expected;
		for (const auto& value: CheckpointValues(*live)) expected.push_back(render(value));
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
			found.push_back(render(value));
			if constexpr (requires { typename Container::key_type; }) {
				const auto& key = [&]() -> const auto& { if constexpr (requires { typename Container::mapped_type; }) return value.first; else return value; }();
				for (int repeat = 0; repeat < 2; ++repeat) if (CheckpointFind(frozen, key) != &value) return std::string(name) + " lookup escaped its frozen generation";
			}
		}
		if constexpr (requires { typename Container::key_type; }) {
			using Key = typename Container::key_type;
			const Key missing = [] { if constexpr (std::is_integral_v<Key>) return Key{-1}; else return Key("missing checkpoint lookup key"); }();
			if (CheckpointContains(frozen, missing)) return std::string(name) + " frozen index invented a value";
		}
		if constexpr (requires { frozen.hash_function(); }) {
			const auto rebuilt = CheckpointRebuildHash(frozen, [](const auto& value) { return value; });
			std::vector<std::string> copied;
			for (const auto& value: rebuilt) copied.push_back(render(value));
			if (copied != expected || rebuilt.bucket_count() != frozen.bucket_count()) return std::string(name) + " changed order or buckets when rebuilt";
		}
		return found == expected ? std::string() : std::string(name) + " changed values or order after destruction";
	};
	struct Collisions {
		size_t operator()(int key) const { return static_cast<size_t>(key % 7); }
		size_t operator()(const CheckpointString& key) const { return std::hash<CheckpointString>{}(key) % 7; }
	};
	std::string mismatch;
	if (!(mismatch = check(static_cast<CheckpointVector<CheckpointString>*>(nullptr), "vector")).empty()) return mismatch;
	if (!(mismatch = check(static_cast<CheckpointDeque<CheckpointString>*>(nullptr), "deque")).empty()) return mismatch;
	if (!(mismatch = check(static_cast<CheckpointList<CheckpointString>*>(nullptr), "list")).empty()) return mismatch;
	if (!(mismatch = check(static_cast<CheckpointQueue<CheckpointString>*>(nullptr), "queue")).empty()) return mismatch;
	if (!(mismatch = check(static_cast<CheckpointSet<CheckpointString, std::greater<CheckpointString>>*>(nullptr), "set")).empty()) return mismatch;
	if (!(mismatch = check(static_cast<CheckpointMap<int, CheckpointString>*>(nullptr), "map")).empty()) return mismatch;
	if (!(mismatch = check(static_cast<CheckpointUnorderedMap<int, CheckpointString, Collisions>*>(nullptr), "hash map")).empty()) return mismatch;
	return check(static_cast<CheckpointUnorderedSet<CheckpointString, Collisions>*>(nullptr), "hash set");
}

std::string CheckpointString::SelfTestMismatch() {
	const std::string before = std::string(513, 'a') + std::string("\0saved", 6);
	const std::string after(1025, 'b');
	std::vector<std::shared_ptr<const CheckpointPagePool::Snapshot>> first, second;
	const CheckpointString* source = nullptr;
	{
		CheckpointNativeStorage::AllocationScope allocation(true);
		auto live = std::allocate_shared<CheckpointString>(CheckpointNativeAllocator<CheckpointString>{}, before);
		source = live.get();
		const std::string& alias = *live;
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
