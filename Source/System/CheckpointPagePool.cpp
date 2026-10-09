#include "CheckpointPagePool.h"

#include "PageWriteFence.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <iterator>
#include <limits>
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
	std::vector<unsigned char> saved;
	std::thread::id simThread = std::this_thread::get_id();
	std::atomic<int64_t> workerUs{0}, simFaultUs{0}, otherFaultUs{0};
	std::atomic<uint64_t> simFaults{0}, otherFaults{0};
	bool completed = false;
	Copy(size_t bytes, size_t pageBytes) : pages(bytes), saved(bytes / pageBytes, 0) {}
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
	bool SavePage(size_t page, bool fault) noexcept {
		const auto started = fault ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		Locked guard(lock);
		const size_t offset = page * pageBytes;
		for (const auto& copy: copies) {
			if (copy->saved[page]) continue;
			std::memcpy(copy->pages.data + offset, live.data + offset, pageBytes);
			copy->saved[page] = 1;
			if (fault) {
				const bool simulation = std::this_thread::get_id() == copy->simThread;
				(simulation ? copy->simFaults : copy->otherFaults).fetch_add(1, std::memory_order_relaxed);
				(simulation ? copy->simFaultUs : copy->otherFaultUs).fetch_add(Since(started), std::memory_order_relaxed);
			}
		}
		return PageWriteFence::OpenCopiedPage(reinterpret_cast<uintptr_t>(live.data + offset), pageBytes);
	}
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

void CheckpointPagePool::Grow(size_t slotBytes, size_t minimumSlots, std::vector<void*>& free) {
	const size_t pageBytes = PageWriteFence::SystemPageBytes();
	if (!slotBytes || !pageBytes || minimumSlots > (std::numeric_limits<size_t>::max() - pageBytes) / slotBytes)
		throw std::length_error("native checkpoint pool is too large");
	const size_t slots = std::max(minimumSlots, (size_t{4} << 20) / slotBytes);
	const size_t bytes = (slots * slotBytes + pageBytes - 1) / pageBytes * pageBytes;
	auto block = std::make_shared<Block>(bytes);
	free.reserve(free.size() + slots);
	m_Blocks.push_back(block);
	for (size_t slot = 0; slot < slots; ++slot) free.push_back(block->live.data + slot * slotBytes);
}

bool CheckpointPagePool::Contains(const void* address) const {
	return std::any_of(m_Blocks.begin(), m_Blocks.end(), [&](const auto& block) { return block->Contains(address, 1); });
}

std::shared_ptr<const CheckpointPagePool::Snapshot> CheckpointPagePool::Freeze() const {
	auto snapshot = std::make_shared<Snapshot>();
	snapshot->m_Parts.reserve(m_Blocks.size());
	try {
		for (const auto& block: m_Blocks) {
			auto copy = std::make_shared<Copy>(block->live.bytes, block->pageBytes);
			Block::Prepared prepared{block.get(), copy};
			if (!PageWriteFence::WatchCopies(block.get(), {block->live.data, block->live.bytes}, Block::OnWrite, block.get(), Block::Arm, &prepared))
				throw std::runtime_error("could not fence native checkpoint pages");
			snapshot->m_Parts.push_back({block, std::move(copy)});
		}
	} catch (...) {
		snapshot->Drain();
		throw;
	}
	std::sort(snapshot->m_Parts.begin(), snapshot->m_Parts.end(), [](const auto& a, const auto& b) {
		return reinterpret_cast<uintptr_t>(a.block->live.data) < reinterpret_cast<uintptr_t>(b.block->live.data);
	});
	return snapshot;
}

bool CheckpointPagePool::Snapshot::Contains(const void* source, size_t bytes) const {
	return std::any_of(m_Parts.begin(), m_Parts.end(), [&](const auto& part) { return part.block->Contains(source, bytes); });
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

void CheckpointPagePool::Snapshot::Drain() const {
	for (const auto& part: m_Parts) {
		const auto start = std::chrono::steady_clock::now();
		for (size_t page = 0; page < part.copy->saved.size(); ++page)
			if (!part.block->SavePage(page, false)) throw std::runtime_error("could not drain native checkpoint pages");
		part.block->Complete(part.copy);
		part.copy->workerUs.fetch_add(Since(start), std::memory_order_relaxed);
	}
}

CheckpointPagePool::Costs CheckpointPagePool::Snapshot::Cost() const {
	Costs result;
	for (const auto& part: m_Parts) {
		result.bytes += part.copy->pages.bytes;
		result.workerUs += part.copy->workerUs.load(std::memory_order_relaxed);
		result.simFaultUs += part.copy->simFaultUs.load(std::memory_order_relaxed);
		result.otherFaultUs += part.copy->otherFaultUs.load(std::memory_order_relaxed);
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
		first = pool.Freeze();
		std::memset(source, 71, expected.size());
		second = pool.Freeze();
		std::memset(source, 99, expected.size());
		if (!first->Read(source, restored.data(), restored.size()) || restored != expected) return "first native page generation differs";
	}
	auto drain = std::async(std::launch::async, [&] { first->Drain(); second->Drain(); });
	drain.get();
	if (!second->Read(source, restored.data(), restored.size()) || !std::all_of(restored.begin(), restored.end(), [](auto byte) { return byte == 71; }))
		return "native page snapshot did not survive its pool";
	if (first->Read(expected.data(), restored.data(), restored.size())) return "native page snapshot accepted an unrelated address";
	return {};
}
