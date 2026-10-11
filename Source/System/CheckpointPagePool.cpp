#include "CheckpointPagePool.h"

#include "PageWriteFence.h"
#include "CheckpointFailure.h"
#include "CheckpointNativeStorage.h"
#include "ScenarioRunner.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <future>
#include <iterator>
#include <limits>
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
	std::thread::id simThread = std::this_thread::get_id();
	std::atomic<int64_t> workerUs{0}, simFaultNs{0}, otherFaultNs{0};
	std::atomic<uint64_t> simFaults{0}, otherFaults{0};
	bool completed = false;
	Copy(size_t bytes, size_t pageBytes) : pages(bytes), saved(bytes / pageBytes) {
		for (auto& page: saved) page.store(0, std::memory_order_relaxed);
	}
};

struct CheckpointPagePool::Block {
	Pages live;
	size_t pageBytes = PageWriteFence::SystemPageBytes();
	std::atomic_flag lock = ATOMIC_FLAG_INIT;
	std::vector<std::shared_ptr<Copy>> copies;
	explicit Block(size_t bytes) : live(bytes) {}
	~Block() { PageWriteFence::UnwatchCopies(this); }
	struct Prepared { Block* block; std::shared_ptr<Copy> copy; };
	static bool Arm(void* context, uintptr_t address, size_t bytes) noexcept {
		auto& prepared = *static_cast<Prepared*>(context);
		auto& block = *prepared.block;
		Locked guard(block.lock);
		try {
			std::erase_if(block.copies, [](const auto& copy) { return copy->completed; });
			block.copies.push_back(prepared.copy);
		} catch (...) { return false; }
		if (PageWriteFence::ProtectCopyPages(address, bytes)) return true;
		block.copies.pop_back();
		return false;
	}
	bool SavePages(size_t first, size_t count, bool fault) noexcept {
		const auto started = fault ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		Locked guard(lock);
		bool savedAny = false;
		for (const auto& copy: copies) {
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
			for (const auto& copy: copies) if (!copy->completed) {
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
		if (std::all_of(copies.begin(), copies.end(), [](const auto& value) { return value->completed; }) &&
		    !PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(live.data), live.bytes))
			throw std::runtime_error("could not open copied native checkpoint pages");
	}
	void Abandon(const std::shared_ptr<Copy>& copy) noexcept {
		Locked guard(lock);
		std::erase(copies, copy);
		if (std::all_of(copies.begin(), copies.end(), [](const auto& value) { return value->completed; }))
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
	auto block = std::make_shared<Block>(bytes);
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
std::shared_ptr<const CheckpointPagePool::Snapshot> CheckpointPagePool::Allocation::Freeze() const {
	auto snapshot = std::make_shared<Snapshot>();
	snapshot->m_Parts.reserve(1);
	auto copy = std::make_shared<Copy>(m_Block->live.bytes, m_Block->pageBytes);
	Block::Prepared prepared{m_Block.get(), copy};
	if (!PageWriteFence::WatchCopies(m_Block.get(), {m_Block->live.data, m_Block->live.bytes}, Block::OnWrite, m_Block.get(), Block::Arm, &prepared))
		throw std::runtime_error("could not fence checkpoint allocation pages");
	snapshot->m_Parts.push_back({m_Block, std::move(copy)});
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
		Block::Prepared prepared{block.get(), copy};
		if (arm && !PageWriteFence::WatchCopies(block.get(), {block->live.data, block->live.bytes}, Block::OnWrite, block.get(), Block::Arm, &prepared))
			throw std::runtime_error("could not fence native checkpoint pages");
		snapshot->m_Parts.push_back({block, std::move(copy)});
	}
	std::sort(snapshot->m_Parts.begin(), snapshot->m_Parts.end(), [](const auto& a, const auto& b) {
		return reinterpret_cast<uintptr_t>(a.block->live.data) < reinterpret_cast<uintptr_t>(b.block->live.data);
	});
	return snapshot;
}

bool CheckpointPagePool::Snapshot::Contains(const void* source, size_t bytes) const {
	const uintptr_t address = reinterpret_cast<uintptr_t>(source);
	const auto end = std::upper_bound(m_Parts.begin(), m_Parts.end(), address, [](uintptr_t address, const Part& part) {
		return address < reinterpret_cast<uintptr_t>(part.block->live.data);
	});
	return end != m_Parts.begin() && std::prev(end)->block->Contains(source, bytes);
}

bool CheckpointPagePool::Snapshot::CanBorrow(const void* source, size_t bytes) const {
	const uintptr_t address = reinterpret_cast<uintptr_t>(source);
	const auto end = std::upper_bound(m_Parts.begin(), m_Parts.end(), address, [](uintptr_t address, const Part& part) {
		return address < reinterpret_cast<uintptr_t>(part.block->live.data);
	});
	if (end == m_Parts.begin()) return false;
	const Part& part = *std::prev(end);
	if (!part.block->Contains(source, bytes)) return false;
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
	const size_t offset = address - reinterpret_cast<uintptr_t>(part.block->live.data);
	for (size_t at = 0; at < bytes;) {
		const size_t page = (offset + at) / part.block->pageBytes;
		if (!part.block->SavePage(page, false)) throw std::runtime_error("could not copy native checkpoint page");
		const size_t count = std::min(bytes - at, part.block->pageBytes - (offset + at) % part.block->pageBytes);
		std::memcpy(static_cast<unsigned char*>(destination) + at, part.copy->pages.data + offset + at, count);
		at += count;
	}
	return true;
}

void CheckpointPagePool::Snapshot::Arm() const {
	for (const auto& part: m_Parts) {
		Block::Prepared prepared{part.block.get(), part.copy};
		if (!PageWriteFence::WatchCopies(part.block.get(), {part.block->live.data, part.block->live.bytes}, Block::OnWrite, part.block.get(), Block::Arm, &prepared))
			throw std::runtime_error("could not fence native checkpoint pages");
	}
}

void CheckpointPagePool::Snapshot::Drain() const {
	for (const auto& part: m_Parts) {
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
		first = pool.Freeze();
		if (!first->CanBorrow(source, expected.size())) return "fresh native pages could not be borrowed";
		std::memset(source, 71, expected.size());
		if (first->CanBorrow(source, expected.size())) return "changed native pages were still borrowed";
		second = pool.Freeze();
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
	std::lock_guard lock(pools->mutex);
	snapshots.reserve(pools->sizes.size());
	for (const auto& pool: pools->sizes) snapshots.push_back(pool.pages.Freeze(false));
	return snapshots;
}

std::string CheckpointNativeStorage::SelfTestMismatch() {
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
	return {};
}
