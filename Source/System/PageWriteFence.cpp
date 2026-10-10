#include "PageWriteFence.h"
#include "CheckpointFailure.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
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

		struct CopyWatch {
			void* owner = nullptr;
			uintptr_t begin = 0, end = 0;
			PageWriteFence::CopyObserver observer = nullptr;
			void* context = nullptr;
			std::atomic<size_t> readers{0};
			std::atomic<bool> retiring{false};
		};
		struct CopyWatches {
			std::map<uintptr_t, std::shared_ptr<CopyWatch>> ranges;
			std::map<void*, std::shared_ptr<CopyWatch>, std::less<void*>> owners;
			std::atomic<size_t> count{0};
			std::atomic_flag lock = ATOMIC_FLAG_INIT;
		};
		CopyWatches& Watches() {
			static CopyWatches watches;
			return watches;
		}
		std::mutex& HandlerMutex() {
			static std::mutex mutex;
			return mutex;
		}

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
		size_t PageBytes() {
			const long bytes = sysconf(_SC_PAGESIZE);
			return bytes > 0 ? static_cast<size_t>(bytes) : 0;
		}

		bool Protect(uintptr_t address, size_t bytes, bool readOnly) {
			return bytes == 0 || mprotect(reinterpret_cast<void*>(address), bytes, readOnly ? PROT_READ : PROT_READ | PROT_WRITE) == 0;
		}

		bool Take(uintptr_t address);

		// Darwin reports a write to a read-only page as SIGBUS, Linux as SIGSEGV; each keeps the handler it had before ours.
		struct sigaction s_PreviousSegv{};
		struct sigaction s_PreviousBus{};

		void OnFault(int signal, siginfo_t* info, void* context) {
			if (info && Take(reinterpret_cast<uintptr_t>(info->si_addr))) {
				return;
			}
			const struct sigaction& previous = signal == SIGBUS ? s_PreviousBus : s_PreviousSegv;
			if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction) {
				previous.sa_sigaction(signal, info, context);
				return;
			}
			if (!(previous.sa_flags & SA_SIGINFO) && previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
				previous.sa_handler(signal);
				return;
			}
			// The default action: the faulting write runs again and ends the process as it would have without the fence.
			struct sigaction fallback{};
			fallback.sa_handler = SIG_DFL;
			sigemptyset(&fallback.sa_mask);
			sigaction(signal, &fallback, nullptr);
		}

		bool InstallOn(int signal, struct sigaction& previous) {
			struct sigaction current{};
			if (sigaction(signal, nullptr, &current) != 0) {
				return false;
			}
			if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == OnFault) {
				return true;
			}
			struct sigaction action{};
			action.sa_sigaction = OnFault;
			action.sa_flags = SA_SIGINFO | SA_ONSTACK;
			sigemptyset(&action.sa_mask);
			return sigaction(signal, &action, &previous) == 0;
		}

		// Checked at every arm, so a handler installed after ours is chained to rather than left to meet the fence.
		bool InstallHandler() {
			return InstallOn(SIGSEGV, s_PreviousSegv) && InstallOn(SIGBUS, s_PreviousBus);
		}
#endif

		// Runs on the faulting thread, before its write lands: the page is copied aside and opened for writing.
		bool Take(uintptr_t address) {
			CopyWatches& copies = Watches();
			if (copies.count.load(std::memory_order_acquire)) {
				CopyWatch* found = nullptr;
				{
					SpinLock guard(copies.lock);
					auto range = copies.ranges.upper_bound(address);
					if (range != copies.ranges.begin()) {
						CopyWatch& watch = *std::prev(range)->second;
						if (!watch.retiring.load(std::memory_order_relaxed) && address < watch.end) {
							watch.readers.fetch_add(1, std::memory_order_acquire);
							found = &watch;
						}
					}
				}
				if (found) {
					const bool taken = found->observer(found->context, address);
					found->readers.fetch_sub(1, std::memory_order_release);
					return taken;
				}
			}
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

	bool PageWriteFence::WatchCopies(void* owner, Buffer buffer, CopyObserver observer, void* context, CopyArm arm, void* armContext, CopyWatchCosts* costs) {
		const auto started = costs ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		const size_t page = PageBytes();
		const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer.data);
		if (!owner || !observer || !page || !buffer.bytes || begin % page || buffer.bytes % page || buffer.bytes > UINTPTR_MAX - begin) return false;
		{
			std::lock_guard setup(HandlerMutex());
			if (!InstallHandler()) return false;
		}
		CopyWatches& copies = Watches();
		std::shared_ptr<CopyWatch> watch;
		decltype(copies.ranges)::node_type rangeNode;
		decltype(copies.owners)::node_type ownerNode;
		// Nodes allocate before the registry lock; the fault handler only looks them up.
		{
			SpinLock guard(copies.lock);
			if (const auto previous = copies.owners.find(owner); previous != copies.owners.end()) watch = previous->second;
		}
		if (!watch) {
			try {
				CheckpointFailure::Check(CheckpointFailure::Point::CopyWatch);
				watch = std::make_shared<CopyWatch>();
				watch->owner = owner; watch->begin = begin; watch->end = begin + buffer.bytes;
				watch->observer = observer; watch->context = context;
				decltype(copies.owners) owners;
				owners.emplace(owner, watch); ownerNode = owners.extract(owner);
				CheckpointFailure::Check(CheckpointFailure::Point::CopyWatch);
				decltype(copies.ranges) ranges;
				ranges.emplace(begin, watch); rangeNode = ranges.extract(begin);
			} catch (const std::bad_alloc&) { return false; }
		}
		{
			SpinLock guard(copies.lock);
			const auto previous = copies.owners.find(owner);
			if (previous != copies.owners.end()) {
				watch = previous->second;
				if (watch->retiring.load(std::memory_order_relaxed) || !arm || watch->begin != begin || begin + buffer.bytes < watch->end || watch->observer != observer || watch->context != context) return false;
			}
			auto next = copies.ranges.lower_bound(begin);
			if (next != copies.ranges.end() && next->second == watch) ++next;
			if (next != copies.ranges.end() && next->first < begin + buffer.bytes) return false;
			const auto first = copies.ranges.lower_bound(begin);
			if (first != copies.ranges.begin() && std::prev(first)->second->end > begin) return false;
			if (previous == copies.owners.end()) {
				if (!rangeNode || !ownerNode) return false;
				copies.ranges.insert(std::move(rangeNode));
				copies.owners.insert(std::move(ownerNode));
				copies.count.fetch_add(1, std::memory_order_release);
			} else watch->end = begin + buffer.bytes;
			watch->readers.fetch_add(1, std::memory_order_acquire);
		}
		// A failed arm retains its observer until unwatch opens any protected pages.
		const auto arming = costs ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		if (costs) costs->setupUs = std::chrono::duration_cast<std::chrono::microseconds>(arming - started).count();
		if (arm) {
			const bool armed = arm(armContext, begin, buffer.bytes);
			if (costs) costs->armUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - arming).count();
			watch->readers.fetch_sub(1, std::memory_order_release);
			return armed;
		}
		const bool protectedPages = Protect(begin, buffer.bytes, true);
		watch->readers.fetch_sub(1, std::memory_order_release);
		if (!protectedPages) UnwatchCopies(owner);
		return protectedPages;
	}

	void PageWriteFence::UnwatchCopies(void* owner) {
		CopyWatches& copies = Watches();
		if (!copies.count.load(std::memory_order_acquire)) return;
		std::shared_ptr<CopyWatch> retired;
		bool first = false;
		{
			SpinLock guard(copies.lock);
			if (const auto found = copies.owners.find(owner); found != copies.owners.end()) {
				retired = found->second;
				first = !retired->retiring.exchange(true, std::memory_order_acq_rel);
			}
		}
		if (!retired) return;
		if (!first) {
			while (retired->retiring.load(std::memory_order_acquire)) std::this_thread::yield();
			return;
		}
		// The observer's context survives every arm and fault that already retained it.
		while (retired->readers.load(std::memory_order_acquire)) std::this_thread::yield();
		Protect(retired->begin, retired->end - retired->begin, false);
		decltype(copies.ranges)::node_type rangeNode;
		decltype(copies.owners)::node_type ownerNode;
		{
			SpinLock guard(copies.lock);
			rangeNode = copies.ranges.extract(retired->begin);
			ownerNode = copies.owners.extract(owner);
			retired->retiring.store(false, std::memory_order_release);
			copies.count.fetch_sub(1, std::memory_order_release);
		}
	}

	size_t PageWriteFence::SystemPageBytes() { return PageBytes(); }
	bool PageWriteFence::OpenCopiedPage(uintptr_t address, size_t bytes) noexcept { return Protect(address, bytes, false); }
	bool PageWriteFence::ProtectCopyPages(uintptr_t address, size_t bytes) noexcept { return Protect(address, bytes, true); }

	bool PageWriteFence::Arm(const std::vector<Buffer>& buffers) {
		State& fence = Fence();
		if (fence.armed.load(std::memory_order_acquire)) {
			// A fence nobody restored keeps its writes, as a copy nobody put back would have.
			Release();
		}
		{
			std::lock_guard setup(HandlerMutex());
			if (!InstallHandler()) return false;
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
		std::lock_guard setup(HandlerMutex());
		return InstallHandler() && PageBytes() != 0;
	}

	bool PageWriteFence::IsArmed() {
		return Fence().armed.load(std::memory_order_acquire);
	}

	uint64_t PageWriteFence::GetFaultCount() {
		return Fence().faults.load(std::memory_order_relaxed);
	}

	std::string PageWriteFence::CopyWatchSelfTestMismatch() {
		if (!IsSupported()) return "unsupported";
		const size_t page = PageBytes();
		struct Pages {
			uint8_t* data;
			size_t bytes;
			~Pages() {
#ifdef _WIN32
				if (data) VirtualFree(data, 0, MEM_RELEASE);
#else
				if (data) munmap(data, bytes);
#endif
			}
		} pages{nullptr, page * 1026};
#ifdef _WIN32
		pages.data = static_cast<uint8_t*>(VirtualAlloc(nullptr, pages.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
		void* mapped = mmap(nullptr, pages.bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapped != MAP_FAILED) pages.data = static_cast<uint8_t*>(mapped);
#endif
		if (!pages.data) return "could not map test pages";
		std::memset(pages.data, 42, pages.bytes);
		struct Probe {
			uint8_t* data;
			std::vector<uint8_t> saved;
			std::promise<void>* entered = nullptr;
			std::shared_future<void> release;
		};
		Probe first{pages.data, std::vector<uint8_t>(page)}, second{pages.data + page, std::vector<uint8_t>(page)};
		const auto observe = +[](void* context, uintptr_t) noexcept {
			auto& probe = *static_cast<Probe*>(context);
			if (probe.entered) {
				probe.entered->set_value();
				probe.release.wait();
			}
			std::memcpy(probe.saved.data(), probe.data, probe.saved.size());
			return OpenCopiedPage(reinterpret_cast<uintptr_t>(probe.data), probe.saved.size());
		};
		if (!WatchCopies(&second, {second.data, page}, observe, &second)) return "second watch refused";
		struct ArmGate {
			std::promise<void> entered;
			std::shared_future<void> release;
		} armGate;
		std::promise<void> releaseArm;
		armGate.release = releaseArm.get_future().share();
		auto armEntered = armGate.entered.get_future();
		const auto arm = +[](void* context, uintptr_t address, size_t bytes) noexcept {
			auto& gate = *static_cast<ArmGate*>(context);
			gate.entered.set_value();
			gate.release.wait();
			return ProtectCopyPages(address, bytes);
		};
		auto armer = std::async(std::launch::async, [&] { return WatchCopies(&first, {first.data, page}, observe, &first, arm, &armGate); });
		const bool entered = armEntered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		bool independent = false;
		std::future<void> writer;
		if (entered) {
			writer = std::async(std::launch::async, [&] { *static_cast<volatile uint8_t*>(second.data) = 77; });
			// The first arm stays blocked until the unrelated fault has answered.
			independent = writer.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		}
		releaseArm.set_value();
		const bool armed = armer.get();
		if (writer.valid()) writer.get();
		UnwatchCopies(&first);
		UnwatchCopies(&second);
		if (!entered || !armed || !independent || second.saved.front() != 42 || second.data[0] != 77) return "one heap arm blocked another heap's frozen write";

		std::promise<void> observerEntered, releaseObserver, unwatchEntered;
		second.entered = &observerEntered;
		second.release = releaseObserver.get_future().share();
		auto observed = observerEntered.get_future();
		auto retiring = unwatchEntered.get_future();
		if (!WatchCopies(&second, {second.data, page}, observe, &second)) return "retained watch refused";
		writer = std::async(std::launch::async, [&] { *static_cast<volatile uint8_t*>(second.data) = 91; });
		const bool observedWrite = observed.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
		auto unwatch = std::async(std::launch::async, [&] { unwatchEntered.set_value(); UnwatchCopies(&second); });
		retiring.wait();
		const bool releasedEarly = unwatch.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
		releaseObserver.set_value();
		writer.get();
		unwatch.get();
		if (!observedWrite || releasedEarly || second.saved.front() != 77 || second.data[0] != 91) return "unwatch released a context still answering its frozen write";
		std::vector<Probe> many;
		many.reserve(1024);
		struct UnwatchAll {
			std::vector<Probe>& probes;
			~UnwatchAll() { for (auto& probe: probes) UnwatchCopies(&probe); }
		} unwatchAll{many};
		for (size_t index = 0; index < 1024; ++index) {
			many.push_back({pages.data + page * (index + 2), std::vector<uint8_t>(page)});
			auto& probe = many.back();
			if (!WatchCopies(&probe, {probe.data, page}, observe, &probe)) return "independent page watch refused at " + std::to_string(index);
		}
		for (auto& probe: many) {
			*static_cast<volatile uint8_t*>(probe.data) = 83;
			if (probe.saved.front() != 42) return "page watch lookup preserved another allocation";
			UnwatchCopies(&probe);
		}
		Probe retry{pages.data, std::vector<uint8_t>(page)};
		for (size_t after: {size_t(0), size_t(1)}) {
			CheckpointFailure::Scope failure(CheckpointFailure::Point::CopyWatch, after);
			if (WatchCopies(&retry, {retry.data, page}, observe, &retry) || !failure.Triggered()) return "watch allocation failure was not refused";
			*static_cast<volatile uint8_t*>(retry.data) = 97;
		}
		if (!WatchCopies(&retry, {retry.data, page}, observe, &retry)) return "watch allocation failure left an unusable registration";
		*static_cast<volatile uint8_t*>(retry.data) = 101;
		UnwatchCopies(&retry);
		if (retry.saved.front() != 97 || retry.data[0] != 101) return "watch allocation failure changed the retry's saved page";
		return {};
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
