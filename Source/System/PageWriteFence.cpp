#include "PageWriteFence.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace RTE {

	namespace {
		struct Region {
			uintptr_t begin = 0;
			uintptr_t end = 0;
			uintptr_t pagesBegin = 0;
			uintptr_t pagesEnd = 0;
			std::vector<uint8_t> shadow;
			std::vector<uint8_t> saved;
			std::vector<uint8_t> head;
			std::vector<uint8_t> tail;
		};

		struct State {
			std::vector<Region> regions;
			std::vector<std::pair<size_t, size_t>> written;
			size_t pageBytes = 0;
			std::atomic<bool> armed{false};
			std::atomic_flag lock = ATOMIC_FLAG_INIT;
			std::atomic<uint64_t> faults{0};
		};

		State& Fence() {
			static State state;
			return state;
		}

		struct SpinLock {
			explicit SpinLock(std::atomic_flag& flag) : flag(flag) {
				while (flag.test_and_set(std::memory_order_acquire)) {}
			}
			~SpinLock() { flag.clear(std::memory_order_release); }
			std::atomic_flag& flag;
		};

#ifdef _WIN32
		size_t PageBytes() {
			SYSTEM_INFO info;
			GetSystemInfo(&info);
			return info.dwPageSize;
		}

		bool Protect(uintptr_t address, size_t bytes, bool readOnly) {
			DWORD previous = 0;
			return bytes == 0 || VirtualProtect(reinterpret_cast<void*>(address), bytes, readOnly ? PAGE_READONLY : PAGE_READWRITE, &previous) != 0;
		}

		bool Take(uintptr_t address);

		LONG CALLBACK OnAccessViolation(EXCEPTION_POINTERS* info) {
			const EXCEPTION_RECORD* record = info->ExceptionRecord;
			// Only a write into a fenced page is ours; everything else goes on to the next handler.
			if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 && record->ExceptionInformation[0] == 1 && Take(record->ExceptionInformation[1])) {
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}

		bool InstallHandler() {
			static const bool installed = AddVectoredExceptionHandler(1, OnAccessViolation) != nullptr;
			return installed;
		}
#else
		// No fence on this platform yet: the caller copies the buffers.
		size_t PageBytes() { return 0; }
		bool Protect(uintptr_t, size_t bytes, bool) { return bytes == 0; }
		bool InstallHandler() { return false; }
#endif

		// Runs on the faulting thread, before its write lands: the page is copied aside and opened for writing.
		[[maybe_unused]] bool Take(uintptr_t address) {
			State& fence = Fence();
			if (!fence.armed.load(std::memory_order_acquire)) {
				return false;
			}
			SpinLock guard(fence.lock);
			for (size_t index = 0; index < fence.regions.size(); ++index) {
				Region& region = fence.regions[index];
				if (address < region.pagesBegin || address >= region.pagesEnd) {
					continue;
				}
				const size_t page = (address - region.pagesBegin) / fence.pageBytes;
				const uintptr_t pageAddress = region.pagesBegin + page * fence.pageBytes;
				// Two threads can fault on one page; the second finds it saved and only needs it open.
				if (!region.saved[page]) {
					std::memcpy(region.shadow.data() + page * fence.pageBytes, reinterpret_cast<const void*>(pageAddress), fence.pageBytes);
					region.saved[page] = 1;
					fence.written.emplace_back(index, page);
					fence.faults.fetch_add(1, std::memory_order_relaxed);
				}
				return Protect(pageAddress, fence.pageBytes, false);
			}
			return false;
		}

		void OpenAll(State& fence) {
			for (const Region& region: fence.regions) {
				Protect(region.pagesBegin, region.pagesEnd - region.pagesBegin, false);
			}
		}
	} // namespace

	bool PageWriteFence::Arm(const std::vector<Buffer>& buffers) {
		State& fence = Fence();
		if (fence.armed.load(std::memory_order_acquire)) {
			// A fence nobody restored keeps its writes, as a copy nobody put back would have.
			Release();
		}
		if (!InstallHandler()) {
			return false;
		}
		fence.pageBytes = PageBytes();
		if (fence.pageBytes == 0) {
			return false;
		}
		const uintptr_t mask = fence.pageBytes - 1;
		fence.regions.resize(buffers.size());
		size_t pages = 0;
		for (size_t index = 0; index < buffers.size(); ++index) {
			Region& region = fence.regions[index];
			region.begin = reinterpret_cast<uintptr_t>(buffers[index].data);
			region.end = region.begin + (buffers[index].data ? buffers[index].bytes : 0);
			region.pagesBegin = std::min((region.begin + mask) & ~mask, region.end);
			region.pagesEnd = std::max(region.end & ~mask, region.pagesBegin);
			const size_t count = (region.pagesEnd - region.pagesBegin) / fence.pageBytes;
			region.shadow.resize(count * fence.pageBytes);
			region.saved.assign(count, 0);
			region.head.assign(reinterpret_cast<const uint8_t*>(region.begin), reinterpret_cast<const uint8_t*>(region.pagesBegin));
			region.tail.assign(reinterpret_cast<const uint8_t*>(region.pagesEnd), reinterpret_cast<const uint8_t*>(region.end));
			pages += count;
		}
		fence.written.clear();
		// Every page could fault; the handler never allocates.
		fence.written.reserve(pages);
		fence.armed.store(true, std::memory_order_release);
		for (const Region& region: fence.regions) {
			if (!Protect(region.pagesBegin, region.pagesEnd - region.pagesBegin, true)) {
				OpenAll(fence);
				fence.armed.store(false, std::memory_order_release);
				return false;
			}
		}
		return true;
	}

	bool PageWriteFence::Covers(const std::vector<Buffer>& buffers) {
		const State& fence = Fence();
		if (!fence.armed.load(std::memory_order_acquire) || fence.regions.size() != buffers.size()) {
			return false;
		}
		for (size_t index = 0; index < buffers.size(); ++index) {
			const uintptr_t begin = reinterpret_cast<uintptr_t>(buffers[index].data);
			if (fence.regions[index].begin != begin || fence.regions[index].end != begin + (buffers[index].data ? buffers[index].bytes : 0)) {
				return false;
			}
		}
		return true;
	}

	size_t PageWriteFence::Restore() {
		State& fence = Fence();
		if (!fence.armed.load(std::memory_order_acquire)) {
			return 0;
		}
		// Open first, so putting the pages back cannot fault.
		OpenAll(fence);
		size_t restored = 0;
		{
			SpinLock guard(fence.lock);
			for (const auto& [index, page]: fence.written) {
				Region& region = fence.regions[index];
				std::memcpy(reinterpret_cast<void*>(region.pagesBegin + page * fence.pageBytes), region.shadow.data() + page * fence.pageBytes, fence.pageBytes);
				region.saved[page] = 0;
			}
			restored = fence.written.size();
			fence.written.clear();
			for (const Region& region: fence.regions) {
				std::copy(region.head.begin(), region.head.end(), reinterpret_cast<uint8_t*>(region.begin));
				std::copy(region.tail.begin(), region.tail.end(), reinterpret_cast<uint8_t*>(region.pagesEnd));
			}
			fence.armed.store(false, std::memory_order_release);
		}
		return restored;
	}

	void PageWriteFence::Release() {
		State& fence = Fence();
		if (!fence.armed.load(std::memory_order_acquire)) {
			return;
		}
		OpenAll(fence);
		SpinLock guard(fence.lock);
		for (const auto& [index, page]: fence.written) {
			fence.regions[index].saved[page] = 0;
		}
		fence.written.clear();
		fence.armed.store(false, std::memory_order_release);
	}

	bool PageWriteFence::IsSupported() {
		return InstallHandler() && PageBytes() != 0;
	}

	bool PageWriteFence::IsArmed() {
		return Fence().armed.load(std::memory_order_acquire);
	}

	uint64_t PageWriteFence::GetFaultCount() {
		return Fence().faults.load(std::memory_order_relaxed);
	}

	std::string PageWriteFence::SelfTestMismatch() {
		if (!IsSupported()) {
			return "unsupported";
		}
		const size_t page = PageBytes();
		// Two buffers that start and end mid-page, so both edges and whole pages are written.
		constexpr size_t offset = 37;
		const size_t sizes[2] = {page * 9 + 101, page * 3 + 7};
		std::vector<std::unique_ptr<uint8_t[]>> storage;
		std::vector<Buffer> buffers;
		std::vector<std::vector<uint8_t>> original;
		for (size_t index = 0; index < 2; ++index) {
			storage.emplace_back(new uint8_t[sizes[index] + offset]);
			uint8_t* data = storage.back().get() + offset;
			for (size_t byte = 0; byte < sizes[index]; ++byte) {
				data[byte] = static_cast<uint8_t>((byte * 131 + index * 17) & 0xFF);
			}
			buffers.push_back({data, sizes[index]});
			original.emplace_back(data, data + sizes[index]);
		}
		const auto differs = [&](const char* when) -> std::string {
			for (size_t index = 0; index < buffers.size(); ++index) {
				if (!std::equal(original[index].begin(), original[index].end(), buffers[index].data)) {
					return std::string(when) + " buffer=" + std::to_string(index);
				}
			}
			return {};
		};
		for (int round = 0; round < 2; ++round) {
			const uint64_t faultsBefore = GetFaultCount();
			if (!Arm(buffers)) {
				return "arm refused";
			}
			if (!Covers(buffers)) {
				Release();
				return "armed fence does not cover its buffers";
			}
			// The first byte, the last byte, two bytes of one page, a run across a page boundary and a write in the second buffer.
			buffers[0].data[0] ^= 0xFF;
			buffers[0].data[sizes[0] - 1] ^= 0xFF;
			buffers[0].data[page * 4 + 11] ^= 0xFF;
			buffers[0].data[page * 4 + 12] ^= 0xFF;
			std::memset(buffers[0].data + page * 6 - 5, 0xAB, 10);
			buffers[1].data[page + 3] ^= 0xFF;
			const size_t restored = Restore();
			const uint64_t faults = GetFaultCount() - faultsBefore;
			if (const std::string mismatch = differs("after restore"); !mismatch.empty()) {
				return mismatch + " round=" + std::to_string(round);
			}
			// Page 4, pages 5 and 6 (the run crosses into 6 from 5), and one page of the second buffer; edges are not faults.
			if (restored != faults || faults < 3 || faults > 5) {
				return "faults=" + std::to_string(faults) + " restored=" + std::to_string(restored) + " round=" + std::to_string(round);
			}
		}
		// Released, a fence keeps what was written and leaves the pages writable.
		if (!Arm(buffers)) {
			return "arm refused";
		}
		buffers[0].data[page * 2] ^= 0xFF;
		Release();
		buffers[0].data[page * 3] ^= 0xFF;
		if (IsArmed() || buffers[0].data[page * 2] == original[0][page * 2] || buffers[0].data[page * 3] == original[0][page * 3]) {
			return "release did not keep the writes";
		}
		return {};
	}
} // namespace RTE
