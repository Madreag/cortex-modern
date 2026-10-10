#include "FloatingPointEnvironment.h"
#include "Writer.h"
#include "System.h"
#include "CheckpointArchive.h"
#include "CheckpointProperties.h"
#include "CheckpointPagePool.h"
#include "CheckpointImage.h"
#include "CheckpointNativeSnapshot.h"
#include "ThreadMan.h"
#include "PresetMan.h"
#include "CaptureSentinel.h"
#include "BitmapCheckpoint.h"
#include "Base64/base64.h"
#include "SceneLayer.h"
#include "Scene.h"
#include "MOPixel.h"
#include "Actor.h"
#include "GATutorial.h"
#include "Controller.h"
#include "RTETools.h"
#include "AtomGroup.h"
#include "Deployment.h"
#include "Reader.h"
#include "Timer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <format>
#include <iomanip>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <atomic>
#include <unordered_set>
#include <utility>
#include <future>
#include <iostream>
#include <locale>
#include <variant>

#ifdef _WIN32
#include <windows.h>
#undef AddAtom
#undef GetClassName
#undef LoadBitmap
#endif

using namespace RTE;

std::string RTE::CheckpointFieldText(const std::function<std::string()>& observe) {
	return CheckpointWriter::CaptureNative(observe).Text();
}

namespace {
	struct CheckpointThreadPerformance {
#ifdef _WIN32
		struct Policy { ULONG version = 1, control = 0, state = 0; };
		using Information = BOOL (WINAPI*)(HANDLE, int, void*, DWORD);
		Information get = reinterpret_cast<Information>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadInformation"));
		Information set = reinterpret_cast<Information>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation"));
		Policy previous;
		bool changed = false;
#endif
		size_t depth = 0;
		void Enter() noexcept {
			if (depth++) return;
#ifdef _WIN32
			// A joined capture has a frame deadline even when its window is hidden.
			if (get && set && get(GetCurrentThread(), 3, &previous, sizeof(previous))) {
				Policy performance{1, 1, 0};
				changed = set(GetCurrentThread(), 3, &performance, sizeof(performance)) != FALSE;
			}
#endif
		}
		void Leave() noexcept {
			if (--depth) return;
#ifdef _WIN32
			if (changed) { set(GetCurrentThread(), 3, &previous, sizeof(previous)); changed = false; }
#endif
		}
	};
	thread_local CheckpointThreadPerformance s_CheckpointPerformance;
	struct LeaveCheckpointPerformance {
		~LeaveCheckpointPerformance() { s_CheckpointPerformance.Leave(); }
	};

	using CaptureValue = CheckpointBuffer::ValueKind;
	template<class T> T ReadCaptureValue(std::string_view values, size_t& cursor) {
		if (sizeof(T) > values.size() - cursor) throw std::logic_error("truncated owned checkpoint values");
		T value;
		std::memcpy(&value, values.data() + cursor, sizeof(value));
		cursor += sizeof(value);
		return value;
	}
	std::string_view ReadCaptureString(std::string_view values, size_t& cursor) {
		const uint64_t size = ReadCaptureValue<uint64_t>(values, cursor);
		if (size > values.size() - cursor) throw std::logic_error("truncated owned checkpoint string");
		const std::string_view text = values.substr(cursor, static_cast<size_t>(size));
		cursor += static_cast<size_t>(size);
		return text;
	}
	template<class T> void AppendCaptureNumber(std::string& text, T number) {
		char buffer[64];
		const auto result = [&] {
			if constexpr (std::is_floating_point_v<T>) return ToCharsExact(buffer, buffer + sizeof(buffer), number);
			else return std::to_chars(buffer, buffer + sizeof(buffer), number);
		}();
		if (result.ec != std::errc{}) throw std::runtime_error("could not format checkpoint value");
		text.append(buffer, result.ptr);
	}
	std::string CanonicalCaptureValues(std::string_view values) {
		std::string result;
		size_t cursor = 0;
		while (cursor < values.size()) {
			const size_t begin = cursor;
			const auto kind = ReadCaptureValue<CaptureValue>(values, cursor);
			switch (kind) {
				case CaptureValue::PrimitiveBlock: {
					const auto decode = ReadCaptureValue<CheckpointBuffer::PrimitiveDecoder>(values, cursor);
					decode(result, ReadCaptureString(values, cursor), true);
					continue;
				}
				case CaptureValue::Raw:
				case CaptureValue::String: ReadCaptureString(values, cursor); break;
				case CaptureValue::Property: ReadCaptureValue<int>(values, cursor); ReadCaptureString(values, cursor); break;
				case CaptureValue::NewLine: ReadCaptureValue<int>(values, cursor); ReadCaptureValue<int>(values, cursor); break;
				case CaptureValue::Float: ReadCaptureValue<float>(values, cursor); break;
				case CaptureValue::ElapsedSimTime: ReadCaptureValue<int64_t>(values, cursor); ReadCaptureValue<double>(values, cursor); break;
				case CaptureValue::PeerBegin:
				case CaptureValue::PeerEnd:
				case CaptureValue::SizedRunBegin:
				case CaptureValue::SizedRunEnd: break;
				case CaptureValue::Integer:
				case CaptureValue::Unsigned:
				case CaptureValue::SpacedInteger:
				case CaptureValue::SpacedUnsigned:
				case CaptureValue::Double:
				case CaptureValue::Child:
				case CaptureValue::SizedChild:
				case CaptureValue::Base64:
				case CaptureValue::UrlBase64:
				case CaptureValue::GraphString: ReadCaptureValue<uint64_t>(values, cursor); break;
				default: throw std::logic_error("unknown owned checkpoint value");
			}
			result.append(values.substr(begin, cursor - begin));
		}
		return result;
	}
}

namespace {
	// Arena leases retain only the backing blocks their records use.
	std::atomic<size_t> s_CheckpointPoolLiveBytes{0};
	class CheckpointArenaGroup {
	public:
		struct Block {
			std::pmr::memory_resource* upstream = std::pmr::get_default_resource();
			void* address;
			size_t bytes, alignment;
			Block(size_t count, size_t align) : address(upstream->allocate(count, align)), bytes(count), alignment(align) {
				s_CheckpointPoolLiveBytes.fetch_add(bytes, std::memory_order_relaxed);
			}
			~Block() {
				upstream->deallocate(address, bytes, alignment);
				s_CheckpointPoolLiveBytes.fetch_sub(bytes, std::memory_order_relaxed);
			}
		};
		struct Prepared {
			std::mutex mutex;
			std::vector<std::shared_ptr<Block>> blocks;
			std::atomic<bool> ready{false};
			std::shared_future<void> task;
			std::promise<void> completion;
		};
		explicit CheckpointArenaGroup(const std::shared_ptr<Prepared>& prepared = {}) : m_Prepared(prepared) {}
		void* Allocate(size_t count, size_t alignment, std::vector<std::shared_ptr<Block>>& leases) {
			constexpr size_t chunkBytes = 1 << 20;
			if (count > chunkBytes) {
				auto block = MakeBlock(count, alignment);
				void* address = block->address;
				leases.push_back(std::move(block));
				return address;
			}
			void* address = Bump(count, alignment);
			if (leases.empty() || leases.back() != m_Current) leases.push_back(m_Current);
			return address;
		}
		std::shared_ptr<void> AllocateBytes(size_t count, size_t alignment = alignof(uint8_t)) {
			if (count > (1 << 20)) {
				auto block = MakeBlock(count, alignment);
				return {block, block->address};
			}
			void* address = Bump(count, alignment);
			return {m_Current, address};
		}
		size_t Blocks() const { return m_Blocks.load(std::memory_order_relaxed); }
		size_t Bytes() const { return m_Bytes.load(std::memory_order_relaxed); }
	private:
		std::shared_ptr<Block> m_Current;
		std::weak_ptr<Prepared> m_Prepared;
		size_t m_Used = 0;
		std::atomic<size_t> m_Blocks{0}, m_Bytes{0};
		void* Bump(size_t count, size_t alignment) {
			void* address = m_Current ? static_cast<char*>(m_Current->address) + m_Used : nullptr;
			size_t available = m_Current ? m_Current->bytes - m_Used : 0;
			if (!address || !std::align(alignment, count, address, available)) {
				m_Current = MakeBlock(1 << 20, std::max(alignment, alignof(std::max_align_t)));
				m_Used = 0;
				address = m_Current->address;
			}
			m_Used = static_cast<char*>(address) - static_cast<char*>(m_Current->address) + count;
			return address;
		}
		std::shared_ptr<Block> MakeBlock(size_t bytes, size_t alignment) {
			std::shared_ptr<Block> block;
			if (bytes == (1 << 20) && alignment <= alignof(std::max_align_t)) {
				if (auto prepared = m_Prepared.lock(); prepared && prepared->ready.load(std::memory_order_acquire)) {
					std::lock_guard lock(prepared->mutex);
					if (!prepared->blocks.empty()) { block = std::move(prepared->blocks.back()); prepared->blocks.pop_back(); }
				}
			}
			if (!block) block = std::make_shared<Block>(bytes, alignment);
			m_Blocks.fetch_add(1, std::memory_order_relaxed);
			m_Bytes.fetch_add(bytes, std::memory_order_relaxed);
			return block;
		}
	};
	class CheckpointArenaSource : public std::pmr::memory_resource {
	public:
		explicit CheckpointArenaSource(std::shared_ptr<CheckpointArenaGroup> group) : m_Group(std::move(group)) {}
	private:
		std::shared_ptr<CheckpointArenaGroup> m_Group;
		std::pmr::memory_resource* m_Default = std::pmr::get_default_resource();
		std::vector<std::shared_ptr<CheckpointArenaGroup::Block>> m_Blocks;
		void* do_allocate(size_t count, size_t alignment) override {
			if (!m_Group) return m_Default->allocate(count, alignment);
			return m_Group->Allocate(count, alignment, m_Blocks);
		}
		void do_deallocate(void* address, size_t count, size_t alignment) override {
			if (!m_Group) m_Default->deallocate(address, count, alignment);
		}
		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
	};
	struct CheckpointArenaPool {
		bool releasePreparedOnWorker = false;
		std::shared_ptr<CheckpointArenaGroup::Prepared> prepared;
		std::mutex mutex;
		std::unordered_map<std::thread::id, std::shared_ptr<CheckpointArenaGroup>> groups;
	};
	std::mutex s_ArenaPoolMutex;
	std::shared_ptr<CheckpointArenaPool> s_ArenaPool;
	std::shared_ptr<CheckpointArenaGroup::Prepared> s_NextPrepared;
	std::vector<std::shared_future<void>> s_PreparedReleaseTasks;
	std::atomic<size_t> s_PreparedReleaseWorkerCalls{0}, s_PreparedReleaseWorkerBytes{0};
	size_t s_ArenaPoolUsers = 0;
	uint64_t s_ArenaPoolNext = 0;
	std::atomic<uint64_t> s_ArenaPoolEpoch{0};

	void ReleasePreparedStorage(std::shared_ptr<CheckpointArenaGroup::Prepared> prepared) {
		try {
			std::erase_if(s_PreparedReleaseTasks, [](auto& task) { return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
			auto completion = std::make_shared<std::promise<void>>();
			s_PreparedReleaseTasks.push_back(completion->get_future().share());
			const auto owner = std::this_thread::get_id();
			g_ThreadMan.GetBackgroundThreadPool().push_task([prepared = std::move(prepared), completion, owner]() mutable {
				prepared->task.wait();
				size_t bytes = 0;
				for (const auto& block: prepared->blocks) bytes += block->bytes;
				const auto started = std::chrono::steady_clock::now();
				prepared.reset(); // The pool keeps idle task closures, so release here.
				const bool worker = std::this_thread::get_id() != owner;
				if (worker) {
					s_PreparedReleaseWorkerCalls.fetch_add(1, std::memory_order_relaxed);
					s_PreparedReleaseWorkerBytes.fetch_add(bytes, std::memory_order_relaxed);
				}
				if (const char* value = std::getenv("CCCP_CHECKPOINT_STORAGE_RELEASE"); value && std::string_view(value) == "1") {
					System::PrintDiagnosticLine(std::format("[checkpoint-storage-release] worker={} bytes={} release_us={}", worker ? 1 : 0, bytes,
					    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
				}
				completion->set_value();
			});
		} catch (...) {
			// Queue allocation failure preserves capture and releases locally.
			prepared.reset();
		}
	}

	std::shared_ptr<CheckpointArenaGroup> CurrentArenaGroup() {
		const uint64_t epoch = s_ArenaPoolEpoch.load(std::memory_order_acquire);
		if (!epoch) return {};
		thread_local uint64_t previousEpoch = 0;
		thread_local std::weak_ptr<CheckpointArenaGroup> previousGroup;
		if (epoch == previousEpoch) if (auto group = previousGroup.lock()) return group;
		std::shared_ptr<CheckpointArenaPool> pool;
		{
			std::lock_guard lock(s_ArenaPoolMutex);
			pool = s_ArenaPool;
		}
		if (!pool) return {};
		std::lock_guard lock(pool->mutex);
		auto& group = pool->groups[std::this_thread::get_id()];
		if (!group) group = std::make_shared<CheckpointArenaGroup>(pool->prepared);
		previousEpoch = epoch;
		previousGroup = group;
		return group;
	}
}

struct RTE::CheckpointArena {
	class AllocationReceipts : public std::pmr::memory_resource {
	public:
		size_t bytes = 0, blocks = 0;
		explicit AllocationReceipts(std::pmr::memory_resource* upstream) : m_Upstream(upstream) {}
	private:
		std::pmr::memory_resource* m_Upstream;
		void* do_allocate(size_t count, size_t alignment) override {
			void* result = m_Upstream->allocate(count, alignment);
			bytes += count;
			++blocks;
			return result;
		}
		void do_deallocate(void* address, size_t count, size_t alignment) override {
			m_Upstream->deallocate(address, count, alignment);
		}
		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
	};
	std::atomic<size_t> owners{1};
	std::shared_ptr<CheckpointArenaGroup> group = CurrentArenaGroup();
	CheckpointArenaSource source{group};
	std::pmr::memory_resource* upstream = group ? &source : std::pmr::get_default_resource();
	AllocationReceipts receipts{upstream};
	std::pmr::monotonic_buffer_resource fallback{16384, CaptureTrace::Active() ? &receipts : upstream};
	std::pmr::memory_resource* storage = group ? (CaptureTrace::Active() ? &receipts : upstream) : &fallback;
	size_t nodes = 0, valueBytes = 0, valueCapacity = 0, childBytes = 0, childCapacity = 0;
	void RetainBlock() { owners.fetch_add(1, std::memory_order_relaxed); }
	void ReleaseBlock() noexcept { if (owners.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this; }
};

namespace {
	template<class T> struct CheckpointAllocator {
		using value_type = T;
		CheckpointArena* arena;
		explicit CheckpointAllocator(CheckpointArena* owner) : arena(owner) {}
		template<class U> CheckpointAllocator(const CheckpointAllocator<U>& other) : arena(other.arena) {}
		T* allocate(size_t count) {
			if (count > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
			T* block = static_cast<T*>(arena->storage->allocate(count * sizeof(T), alignof(T)));
			arena->RetainBlock();
			return block;
		}
		// Allocator copies borrow an address while the control block retains its storage.
		void deallocate(T*, size_t) noexcept { arena->ReleaseBlock(); }
		template<class U> bool operator==(const CheckpointAllocator<U>& other) const noexcept { return arena == other.arena; }
	};
}

struct CheckpointBuffer::ValueChunk {
	ValueChunk* next = nullptr;
	size_t size = 0, capacity;
	explicit ValueChunk(size_t bytes) : capacity(bytes) {}
	char* Bytes() { return reinterpret_cast<char*>(this + 1); }
	const char* Bytes() const { return reinterpret_cast<const char*>(this + 1); }
};

struct CheckpointText::Data {
	struct Legacy;
	struct WriterFields {
		std::once_flag ready;
		std::function<CheckpointText()> produce;
		CheckpointText values;
		const CheckpointText& Read() {
			std::call_once(ready, [this] { values = produce(); produce = {}; });
			return values;
		}
	};
	struct Formatting {
		std::mutex ready;
		std::atomic<bool> formatted{false};
		std::string text;
		std::optional<std::string> sharedValues;
		std::vector<std::pair<size_t, size_t>> peerRuns;
		std::once_flag copied;
		std::string tape;
	};
	static std::shared_ptr<Data> Create();
	explicit Data(std::pmr::memory_resource* resource = std::pmr::get_default_resource()) : values(resource), children(resource), ownedBlocks(resource) {}
	struct Pair {
		const Data* current;
		const Data* previous;
		bool operator==(const Pair&) const = default;
	};
	struct PairHash {
		size_t operator()(const Pair& pair) const {
			const size_t first = std::hash<const Data*>{}(pair.current), second = std::hash<const Data*>{}(pair.previous);
			return first ^ (second + 0x9e3779b9u + (first << 6) + (first >> 2));
		}
	};
	std::pmr::string values;
	const CheckpointBuffer::ValueChunk* chunks = nullptr;
	size_t valueSize = 0;
	std::pmr::vector<CheckpointText> children;
	std::pmr::vector<std::shared_ptr<const void>> ownedBlocks;
	// A deferred node's producer, dropped once it has produced: what it captured (a frozen heap, a pixel snapshot) goes with it.
	mutable std::variant<std::monostate, std::function<std::string()>, CapturedProducer, std::function<CheckpointText()>> produce;
	bool deferred = false;
	std::shared_ptr<WriterFields> writerFields;
	int writerDelta = 0;
	std::string peerMark;
	std::string identity;
	size_t ownedBytes = 0;
	bool hasPeer = false;
	bool hasPrimitiveBlocks = false;
	bool usesSimTime = false;
	int64_t simTimeTicks = 0;
	mutable std::atomic<Formatting*> output{nullptr};
	bool ownsOutput = true;
	Formatting& Output() const {
		Formatting* current = output.load(std::memory_order_acquire);
		if (!current) {
			auto candidate = std::make_unique<Formatting>();
			if (output.compare_exchange_strong(current, candidate.get(), std::memory_order_acq_rel, std::memory_order_acquire)) current = candidate.release();
		}
		return *current;
	}
	std::shared_ptr<Data> drainNext;
	std::string_view Values() const {
		if (!chunks) return values;
		if (!chunks->next) return {chunks->Bytes(), chunks->size};
		auto& result = Output();
		// Published chunks are immutable; only a reader joins them into contiguous storage.
		std::call_once(result.copied, [&] {
			std::string tape;
			tape.reserve(valueSize);
			for (auto* chunk = chunks; chunk; chunk = chunk->next) tape.append(chunk->Bytes(), chunk->size);
			result.tape = std::move(tape);
		});
		return result.tape;
	}
	bool SameTape(const Data& other) const {
		struct Cursor {
			const CheckpointBuffer::ValueChunk* next;
			std::string_view part;
			explicit Cursor(const Data& node) : next(node.chunks), part(node.chunks ? std::string_view{} : node.values) { Advance(); }
			void Advance() {
				while (part.empty() && next) { part = {next->Bytes(), next->size}; next = next->next; }
			}
		};
		Cursor left(*this), right(other);
		bool equal = true;
		while (!left.part.empty() && !right.part.empty()) {
			const size_t size = std::min(left.part.size(), right.part.size());
			if (std::memcmp(left.part.data(), right.part.data(), size) != 0) { equal = false; break; }
			left.part.remove_prefix(size); right.part.remove_prefix(size);
			left.Advance(); right.Advance();
		}
		if (equal && left.part.empty() && right.part.empty()) return true;
		if (hasPrimitiveBlocks == other.hasPrimitiveBlocks) return false;
		return CanonicalCaptureValues(Values()) == CanonicalCaptureValues(other.Values());
	}

	~Data() {
		if (ownsOutput) delete output.load(std::memory_order_relaxed);
		std::shared_ptr<Data> pending;
		const auto enqueue = [&pending](Data& node) {
			for (auto& child: node.children) {
				if (child.m_Data && child.m_Data.use_count() == 1) {
					// Only unshared children enter the destruction chain.
					child.m_Data->drainNext = std::move(pending);
					pending = std::move(child.m_Data);
				} else child.m_Data.reset();
			}
		};
		enqueue(*this);
		while (pending) {
			auto node = std::move(pending);
			pending = std::move(node->drainNext);
			enqueue(*node);
			node.reset();
		}
	}
};

struct CheckpointText::Data::Legacy : Data {
	explicit Legacy(std::pmr::memory_resource* resource = std::pmr::get_default_resource()) : Data(resource) {
		ownsOutput = false;
		output.store(&initialOutput, std::memory_order_relaxed);
	}
	Formatting initialOutput;
};

std::shared_ptr<CheckpointText::Data> CheckpointText::Data::Create() {
	if (CheckpointWriter::BatchEnabled() && CheckpointBuffer::s_Arena) {
		const auto& arena = CheckpointBuffer::s_Arena;
		return std::allocate_shared<Data>(CheckpointAllocator<Data>(arena.get()), arena->storage);
	}
	return CheckpointWriter::BatchEnabled() ? std::make_shared<Data>() : std::make_shared<Legacy>();
}

CheckpointText::CheckpointText() = default;

CheckpointText::CheckpointText(std::string text) {
	CheckpointBuffer buffer;
	buffer.Raw(text);
	*this = buffer.Finish();
}

CheckpointText CheckpointText::Deferred(std::function<std::string()> produce, size_t ownedBytes, std::string identity) {
	auto data = Data::Create();
	data->produce = std::move(produce);
	data->deferred = true;
	data->ownedBytes = ownedBytes;
	data->identity = std::move(identity);
	return CheckpointText(std::move(data));
}

CheckpointText CheckpointText::DeferredValues(std::function<CheckpointText()> produce, size_t ownedBytes) {
	auto data = Data::Create();
	data->produce = std::move(produce);
	data->deferred = true;
	data->hasPeer = true;
	data->ownedBytes = ownedBytes;
	return CheckpointText(std::move(data));
}

CheckpointText CheckpointText::DeferredWriter(std::function<CheckpointText()> produce) {
	auto fields = std::make_shared<Data::WriterFields>();
	fields->produce = std::move(produce);
	auto result = DeferredValues([fields] { return fields->Read(); });
	result.m_Data->writerFields = std::move(fields);
	return result;
}

std::pmr::memory_resource* CheckpointText::ProducerStorage() {
	return CheckpointWriter::BatchEnabled() && CheckpointBuffer::s_Arena ? CheckpointBuffer::s_Arena->storage : nullptr;
}

CheckpointText CheckpointText::DeferredOwned(CapturedProducer produce, size_t ownedBytes, std::string identity, std::string peerMark) {
	// The allocation scope survives callable construction even when the batch flag ends.
	auto data = std::allocate_shared<Data>(CheckpointAllocator<Data>(CheckpointBuffer::s_Arena.get()), produce.storage);
	data->produce = std::move(produce);
	data->deferred = true;
	data->ownedBytes = ownedBytes;
	data->identity = std::move(identity);
	data->peerMark = std::move(peerMark);
	data->hasPeer = !data->peerMark.empty();
	return CheckpointText(std::move(data));
}

CheckpointText CheckpointText::DeferredWithPeerRuns(std::function<std::string()> produce, std::string mark, size_t ownedBytes) {
	if (mark.empty()) throw std::logic_error("a per-peer run mark cannot be empty");
	auto data = Data::Create();
	data->produce = std::move(produce);
	data->deferred = true;
	data->peerMark = std::move(mark);
	data->ownedBytes = ownedBytes;
	data->hasPeer = true;
	return CheckpointText(std::move(data));
}

namespace {
	// Drops the marks a producer put around its per-peer runs and remembers where the runs lie in the text that is left.
	void StripPeerMarks(std::string& text, const std::string& mark, std::vector<std::pair<size_t, size_t>>& runs) {
		std::string kept;
		kept.reserve(text.size());
		size_t at = 0, runStart = 0;
		bool inside = false;
		for (size_t found = text.find(mark); found != std::string::npos; found = text.find(mark, at)) {
			kept.append(text, at, found - at);
			if (inside) runs.emplace_back(runStart, kept.size()); else runStart = kept.size();
			inside = !inside;
			at = found + mark.size();
		}
		if (inside) throw std::logic_error("a deferred checkpoint text left a per-peer run open");
		kept.append(text, at, std::string::npos);
		text = std::move(kept);
	}
} // namespace

CheckpointText CheckpointText::Base64(bool url) const {
	CheckpointBuffer buffer;
	buffer.Base64(*this, url);
	return buffer.Finish();
}

CheckpointText CheckpointText::BindSimTime(int64_t ticks) const {
	if (!m_Data || !m_Data->usesSimTime) return *this;
	// The worker binds timer paths while timeless branches keep their published text.
	return Deferred([source = *this, ticks] { return source.AtSimTime(ticks).Text(); }, OwnedBytes());
}

CheckpointText CheckpointText::AtSimTime(int64_t ticks) const {
	if (!m_Data || !m_Data->usesSimTime) return *this;
	struct Frame { const Data* source; std::shared_ptr<Data> bound; size_t next = 0; };
	std::unordered_map<const Data*, std::shared_ptr<Data>> bound;
	std::vector<Frame> pending;
	const auto copy = [&](const Data* source) {
		auto node = Data::Create();
		node->values = source->Values();
		node->ownedBlocks = source->ownedBlocks;
		node->children.reserve(source->children.size());
		node->ownedBytes = source->ownedBytes;
		node->hasPeer = source->hasPeer;
		node->hasPrimitiveBlocks = source->hasPrimitiveBlocks;
		node->simTimeTicks = ticks;
		bound.emplace(source, node);
		pending.push_back({source, node});
		return node;
	};
	const auto root = copy(m_Data.get());
	while (!pending.empty()) {
		Frame& frame = pending.back();
		if (frame.next == frame.source->children.size()) { pending.pop_back(); continue; }
		const auto& child = frame.source->children[frame.next++];
		if (!child.m_Data || !child.m_Data->usesSimTime) {
			frame.bound->children.push_back(child);
			continue;
		}
		const auto found = bound.find(child.m_Data.get());
		if (found != bound.end()) {
			frame.bound->children.emplace_back(CheckpointText(found->second));
			continue;
		}
		const auto parent = frame.bound;
		parent->children.emplace_back(CheckpointText(copy(child.m_Data.get())));
	}
	return CheckpointText(root);
}

size_t CheckpointText::OwnedBytes() const { return m_Data ? m_Data->ownedBytes : 0; }

bool CheckpointText::HasPeerRuns() const { return m_Data && m_Data->hasPeer; }

bool CheckpointText::SameValues(const CheckpointText& other) const {
	if (m_Data == other.m_Data) return true;
	if (!m_Data || !other.m_Data) return false;
	if (m_Data->deferred || other.m_Data->deferred) return m_Data->deferred && other.m_Data->deferred && !m_Data->identity.empty() && m_Data->identity == other.m_Data->identity;
	if (m_Data->children.size() != other.m_Data->children.size()) return false;
	if (m_Data->children.empty()) return m_Data->SameTape(*other.m_Data);
	std::vector<Data::Pair> pending{{m_Data.get(), other.m_Data.get()}};
	std::unordered_set<Data::Pair, Data::PairHash> seen;
	while (!pending.empty()) {
		const auto [current, previous] = pending.back();
		pending.pop_back();
		if (current == previous) continue;
		if (!current || !previous) return false;
		if (current->deferred || previous->deferred) {
			if (!current->deferred || !previous->deferred || current->identity.empty() || current->identity != previous->identity) return false;
			continue;
		}
		if (current->children.size() != previous->children.size() || !current->SameTape(*previous)) return false;
		if (!seen.insert({current, previous}).second) continue;
		for (size_t index = current->children.size(); index > 0; --index) pending.push_back({current->children[index - 1].m_Data.get(), previous->children[index - 1].m_Data.get()});
	}
	return true;
}

CheckpointText CheckpointText::ReuseChildren(const CheckpointText& previous) const {
	if (m_Data == previous.m_Data) return previous;
	if (!m_Data || !previous.m_Data) return *this;
	if (m_Data->deferred || previous.m_Data->deferred) return SameValues(previous) ? previous : *this;
	if (m_Data->children.empty() || previous.m_Data->children.empty()) return SameValues(previous) ? previous : *this;
	struct Result { bool equal; std::shared_ptr<Data> value; };
	struct Frame {
		std::shared_ptr<Data> current, previous;
		size_t next = 0;
		bool entered = false, equal = false, changed = false;
	};
	std::unordered_map<Data::Pair, Result, Data::PairHash> results;
	std::vector<Frame> pending{{m_Data, previous.m_Data}};
	while (!pending.empty()) {
		Frame& frame = pending.back();
		const Data::Pair pair{frame.current.get(), frame.previous.get()};
		if (!frame.entered) {
			if (frame.current == frame.previous) {
				results.emplace(pair, Result{true, frame.previous}); pending.pop_back(); continue;
			}
			if (!frame.current || !frame.previous) {
				results.emplace(pair, Result{false, frame.current}); pending.pop_back(); continue;
			}
			if (frame.current->deferred || frame.previous->deferred) {
				const bool equal = frame.current->deferred && frame.previous->deferred && !frame.current->identity.empty() && frame.current->identity == frame.previous->identity;
				results.emplace(pair, Result{equal, equal ? frame.previous : frame.current}); pending.pop_back(); continue;
			}
			frame.equal = frame.current->children.size() == frame.previous->children.size() && frame.current->SameTape(*frame.previous);
			frame.entered = true;
		}
		const size_t count = std::min(frame.current->children.size(), frame.previous->children.size());
		if (frame.next < count) {
			const auto& currentChild = frame.current->children[frame.next].m_Data;
			const auto& previousChild = frame.previous->children[frame.next].m_Data;
			const auto child = results.find({currentChild.get(), previousChild.get()});
			if (child == results.end()) { pending.push_back({currentChild, previousChild}); continue; }
			frame.equal = frame.equal && child->second.equal;
			frame.changed = frame.changed || child->second.value != currentChild;
			++frame.next;
			continue;
		}
		std::shared_ptr<Data> value = frame.equal ? frame.previous : frame.current;
		if (!frame.equal && frame.changed) {
			value = Data::Create();
			value->values = frame.current->Values();
			value->ownedBlocks = frame.current->ownedBlocks;
			value->children = frame.current->children;
			value->ownedBytes = frame.current->ownedBytes;
			value->hasPeer = frame.current->hasPeer;
			value->hasPrimitiveBlocks = frame.current->hasPrimitiveBlocks;
			value->usesSimTime = frame.current->usesSimTime;
			value->simTimeTicks = frame.current->simTimeTicks;
			for (size_t index = 0; index < count; ++index) {
				const Data::Pair child{frame.current->children[index].m_Data.get(), frame.previous->children[index].m_Data.get()};
				value->children[index] = CheckpointText(results.at(child).value);
			}
		}
		results.emplace(pair, Result{frame.equal, std::move(value)});
		pending.pop_back();
	}
	return CheckpointText(results.at({m_Data.get(), previous.m_Data.get()}).value);
}

CheckpointText CheckpointText::ReindentWriter(int delta) const {
	if (!delta || !m_Data) return *this;
	std::unordered_map<const Data*, CheckpointText> converted;
	const auto convert = [&](const auto& self, const CheckpointText& source) -> CheckpointText {
		if (!source.m_Data) return source;
		if (const auto known = converted.find(source.m_Data.get()); known != converted.end()) return known->second;
		const Data& node = *source.m_Data;
		if (node.writerFields) {
			const int relocated = node.writerDelta + delta;
			auto result = DeferredValues([fields = node.writerFields, relocated] { return fields->Read().ReindentWriter(relocated); });
			result.m_Data->writerFields = node.writerFields;
			result.m_Data->writerDelta = relocated;
			converted.emplace(source.m_Data.get(), result);
			return result;
		}
		if (node.deferred || node.Values().empty()) return source;
		CheckpointBuffer buffer;
		buffer.m_HasPeer = node.hasPeer;
		buffer.m_UsesSimTime = node.usesSimTime;
		buffer.m_SimTimeTicks = node.simTimeTicks;
		buffer.m_HasPrimitiveBlocks = node.hasPrimitiveBlocks;
		buffer.m_OwnedBlocks.assign(node.ownedBlocks.begin(), node.ownedBlocks.end());
		buffer.m_OwnedBlockBytes = node.ownedBytes - node.Values().size();
		for (const auto& child: node.children) buffer.m_OwnedBlockBytes -= child.OwnedBytes();
		const std::string_view values = node.Values();
		size_t cursor = 0;
		while (cursor < values.size()) {
			const size_t first = cursor;
			const auto kind = ReadCaptureValue<CaptureValue>(values, cursor);
			if (kind == CaptureValue::Property) {
				const int oldIndent = ReadCaptureValue<int>(values, cursor);
				const int indent = oldIndent > 0 ? oldIndent + delta : oldIndent;
				if (indent < 0) throw std::logic_error("negative frozen writer indent");
				buffer.Property(ReadCaptureString(values, cursor), indent);
				continue;
			}
			if (kind == CaptureValue::NewLine) {
				const int oldIndent = ReadCaptureValue<int>(values, cursor), count = ReadCaptureValue<int>(values, cursor);
				const int indent = oldIndent > 0 ? oldIndent + delta : oldIndent;
				if (indent < 0) throw std::logic_error("negative frozen writer indent");
				buffer.NewLine(indent, count);
				continue;
			}
			if (kind == CaptureValue::Child || kind == CaptureValue::SizedChild || kind == CaptureValue::Base64 || kind == CaptureValue::UrlBase64 || kind == CaptureValue::GraphString) {
				const auto& child = node.children.at(static_cast<size_t>(ReadCaptureValue<uint64_t>(values, cursor)));
				if (kind == CaptureValue::Child) buffer.Child(self(self, child));
				else if (kind == CaptureValue::SizedChild) buffer.Child(child, true);
				else if (kind == CaptureValue::GraphString) buffer.GraphString(child);
				else buffer.Base64(child, kind == CaptureValue::UrlBase64);
				continue;
			}
			switch (kind) {
				case CaptureValue::Raw: case CaptureValue::String: ReadCaptureString(values, cursor); break;
				case CaptureValue::Integer: case CaptureValue::SpacedInteger: ReadCaptureValue<int64_t>(values, cursor); break;
				case CaptureValue::Unsigned: case CaptureValue::SpacedUnsigned: ReadCaptureValue<uint64_t>(values, cursor); break;
				case CaptureValue::Float: ReadCaptureValue<float>(values, cursor); break;
				case CaptureValue::Double: ReadCaptureValue<double>(values, cursor); break;
				case CaptureValue::ElapsedSimTime: ReadCaptureValue<int64_t>(values, cursor); ReadCaptureValue<double>(values, cursor); break;
				case CaptureValue::PrimitiveBlock: ReadCaptureValue<CheckpointBuffer::PrimitiveDecoder>(values, cursor); ReadCaptureString(values, cursor); break;
				case CaptureValue::PeerBegin: case CaptureValue::PeerEnd: case CaptureValue::SizedRunBegin: case CaptureValue::SizedRunEnd: break;
				default: throw std::logic_error("invalid frozen writer tape");
			}
			buffer.AppendValues(values.substr(first, cursor - first));
		}
		const auto result = buffer.Finish();
		converted.emplace(source.m_Data.get(), result);
		return result;
	};
	return convert(convert, *this);
}

const std::string& CheckpointText::Text() const {
	static const std::string empty;
	if (!m_Data) return empty;
	const auto root = m_Data;
	if (root->Output().formatted.load(std::memory_order_acquire)) return root->Output().text;
	if (root->usesSimTime) {
		std::lock_guard lock(root->Output().ready);
		if (!root->Output().formatted.load(std::memory_order_acquire)) {
			root->Output().text = AtSimTime(root->simTimeTicks).Text();
			root->Output().formatted.store(true, std::memory_order_release);
		}
		return root->Output().text;
	}
	struct Frame { const Data* node; size_t next = 0; };
	std::vector<Frame> pending{{root.get()}};
	while (!pending.empty()) {
		Frame& frame = pending.back();
		const Data* node = frame.node;
		if (node->Output().formatted.load(std::memory_order_acquire)) { pending.pop_back(); continue; }
		if (!node->deferred && frame.next < node->children.size()) {
			const Data* child = node->children[frame.next++].m_Data.get();
			if (child && !child->Output().formatted.load(std::memory_order_acquire)) pending.push_back({child});
			continue;
		}
		const auto format = [node] {
			if (node->deferred) {
				{
					const FloatingPointEnvironment::Scope scope("capture callback");
					node->Output().text = std::visit([node](const auto& produce) -> std::string {
						if constexpr (std::is_same_v<std::remove_cvref_t<decltype(produce)>, std::monostate>) throw std::bad_function_call();
						else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(produce)>, std::function<CheckpointText()>>) {
							const CheckpointText values = produce();
							const std::string& text = values.Text();
							std::string shared = values.SharedText();
							if (shared != text) node->Output().sharedValues = std::move(shared);
							return text;
						}
						else return produce();
					}, node->produce);
				}
				if (!node->peerMark.empty()) StripPeerMarks(node->Output().text, node->peerMark, node->Output().peerRuns);
				node->Output().formatted.store(true, std::memory_order_release);
				node->produce.emplace<std::monostate>();
				return;
			}
			std::string text;
			std::vector<size_t> sizedRuns;
			const std::string_view values = node->Values();
			text.reserve(values.size());
			size_t cursor = 0;
			while (cursor < values.size()) {
				const auto kind = ReadCaptureValue<CaptureValue>(values, cursor);
				switch (kind) {
					case CaptureValue::Raw: text += ReadCaptureString(values, cursor); break;
					case CaptureValue::Integer:
					case CaptureValue::SpacedInteger:
						AppendCaptureNumber(text, ReadCaptureValue<int64_t>(values, cursor));
						if (kind == CaptureValue::SpacedInteger) text.push_back(' ');
						break;
					case CaptureValue::Unsigned:
					case CaptureValue::SpacedUnsigned:
						AppendCaptureNumber(text, ReadCaptureValue<uint64_t>(values, cursor));
						if (kind == CaptureValue::SpacedUnsigned) text.push_back(' ');
						break;
					case CaptureValue::Float: AppendCaptureNumber(text, ReadCaptureValue<float>(values, cursor)); break;
					case CaptureValue::Double: AppendCaptureNumber(text, ReadCaptureValue<double>(values, cursor)); break;
					case CaptureValue::ElapsedSimTime: {
						const int64_t startTicks = ReadCaptureValue<int64_t>(values, cursor);
						const double ticksPerMS = ReadCaptureValue<double>(values, cursor);
						AppendCaptureNumber(text, static_cast<double>(node->simTimeTicks - startTicks) / ticksPerMS);
						break;
					}
					case CaptureValue::String: {
						const auto string = ReadCaptureString(values, cursor);
						AppendCaptureNumber(text, string.size()); text.push_back(' '); text += string; text.push_back(' ');
						break;
					}
					case CaptureValue::Child:
					case CaptureValue::SizedChild:
					case CaptureValue::Base64:
					case CaptureValue::UrlBase64:
					case CaptureValue::GraphString: {
						const auto index = ReadCaptureValue<uint64_t>(values, cursor);
						const auto& data = node->children.at(static_cast<size_t>(index)).m_Data;
						const std::string& child = data ? data->Output().text : empty;
						if (kind == CaptureValue::SizedChild) { AppendCaptureNumber(text, child.size()); text.push_back(' '); }
						if (kind == CaptureValue::GraphString) { text.push_back('s'); AppendCaptureNumber(text, child.size()); text.push_back(':'); }
						if (kind == CaptureValue::Base64 || kind == CaptureValue::UrlBase64) text += base64_encode(child, kind == CaptureValue::UrlBase64);
						else text += child;
						if (kind == CaptureValue::SizedChild) text.push_back(' ');
						break;
					}
					case CaptureValue::NewLine: {
						const int indent = ReadCaptureValue<int>(values, cursor), count = ReadCaptureValue<int>(values, cursor);
						for (int i = 0; i < count; ++i) { text.push_back('\n'); if (indent > 0) text.append(static_cast<size_t>(indent), '\t'); }
						break;
					}
					case CaptureValue::Property: {
						const int indent = ReadCaptureValue<int>(values, cursor);
						text.push_back('\n'); text.append(static_cast<size_t>(indent), '\t'); text += ReadCaptureString(values, cursor); text += " = ";
						break;
					}
					case CaptureValue::PeerBegin:
					case CaptureValue::PeerEnd: break;
					case CaptureValue::PrimitiveBlock: {
						const auto decode = ReadCaptureValue<CheckpointBuffer::PrimitiveDecoder>(values, cursor);
						decode(text, ReadCaptureString(values, cursor), false);
						break;
					}
					case CaptureValue::SizedRunBegin: sizedRuns.push_back(text.size()); break;
					case CaptureValue::SizedRunEnd: {
						if (sizedRuns.empty()) throw std::logic_error("unmatched owned sized run");
						const size_t begin = sizedRuns.back(); sizedRuns.pop_back();
						std::string size; AppendCaptureNumber(size, text.size() - begin); size.push_back(' ');
						text.insert(begin, size); text.push_back(' ');
						break;
					}
					default: throw std::logic_error("unknown owned checkpoint value");
				}
			}
			if (!sizedRuns.empty()) throw std::logic_error("open owned sized run");
			node->Output().text = std::move(text);
			node->Output().formatted.store(true, std::memory_order_release);
		};
		{
			std::lock_guard lock(node->Output().ready);
			if (!node->Output().formatted.load(std::memory_order_acquire)) format();
		}
		pending.pop_back();
	}
	return root->Output().text;
}

CheckpointText CheckpointText::Compact() const {
	if (!m_Data) return {};
	// Completed images keep their bytes for readers, but never need their boundary allocations again.
	auto data = std::make_shared<Data>();
	data->deferred = true;
	data->Output().text = Text();
	if (HasPeerRuns()) {
		std::string shared = SharedText();
		if (shared != data->Output().text) {
			data->hasPeer = true;
			data->Output().sharedValues = std::move(shared);
		}
	}
	data->ownedBytes = data->Output().text.size() + (data->Output().sharedValues ? data->Output().sharedValues->size() : 0);
	data->Output().formatted.store(true, std::memory_order_release);
	return CheckpointText(std::move(data));
}

std::string CheckpointText::SharedText(int64_t simTimeTicks) const {
	if (!m_Data || !m_Data->hasPeer) return BindSimTime(simTimeTicks).Text();
	return m_Data->usesSimTime ? AtSimTime(simTimeTicks).SharedText() : SharedText();
}

std::string CheckpointText::SharedText() const {
	if (!m_Data || !m_Data->hasPeer) return Text();
	if (m_Data->usesSimTime) return AtSimTime(m_Data->simTimeTicks).SharedText();
	if (m_Data->deferred) {
		const std::string& text = Text();
		if (m_Data->Output().sharedValues) return *m_Data->Output().sharedValues;
		std::string shared;
		size_t at = 0;
		for (const auto& [begin, end]: m_Data->Output().peerRuns) {
			shared.append(text, at, begin - at);
			at = end;
		}
		shared.append(text, at, std::string::npos);
		return shared;
	}
	const Data& node = *m_Data;
	const std::string_view values = node.Values();
	std::string text;
	size_t cursor = 0;
	int peer = 0;
	std::vector<std::optional<size_t>> sizedRuns;
	// Appends what the full text would, unless it lies inside a per-peer run.
	const auto put = [&text, &peer](std::string_view part) { if (peer == 0) text += part; };
	const auto number = [&put](auto value) { std::string formatted; AppendCaptureNumber(formatted, value); put(formatted); };
	while (cursor < values.size()) {
		const auto kind = ReadCaptureValue<CaptureValue>(values, cursor);
		switch (kind) {
			case CaptureValue::Raw: put(ReadCaptureString(values, cursor)); break;
			case CaptureValue::Integer:
			case CaptureValue::SpacedInteger:
				number(ReadCaptureValue<int64_t>(values, cursor));
				if (kind == CaptureValue::SpacedInteger) put(" ");
				break;
			case CaptureValue::Unsigned:
			case CaptureValue::SpacedUnsigned:
				number(ReadCaptureValue<uint64_t>(values, cursor));
				if (kind == CaptureValue::SpacedUnsigned) put(" ");
				break;
			case CaptureValue::Float: number(ReadCaptureValue<float>(values, cursor)); break;
			case CaptureValue::Double: number(ReadCaptureValue<double>(values, cursor)); break;
			case CaptureValue::ElapsedSimTime: {
				const int64_t startTicks = ReadCaptureValue<int64_t>(values, cursor);
				const double ticksPerMS = ReadCaptureValue<double>(values, cursor);
				number(static_cast<double>(node.simTimeTicks - startTicks) / ticksPerMS);
				break;
			}
			case CaptureValue::String: {
				const auto string = ReadCaptureString(values, cursor);
				number(string.size()); put(" "); put(string); put(" ");
				break;
			}
			case CaptureValue::Child:
			case CaptureValue::SizedChild:
			case CaptureValue::Base64:
			case CaptureValue::UrlBase64:
			case CaptureValue::GraphString: {
				const auto index = ReadCaptureValue<uint64_t>(values, cursor);
				const std::string child = node.children.at(static_cast<size_t>(index)).SharedText();
				if (kind == CaptureValue::SizedChild) { number(child.size()); put(" "); }
				if (kind == CaptureValue::GraphString) { put("s"); number(child.size()); put(":"); }
				if (kind == CaptureValue::Base64 || kind == CaptureValue::UrlBase64) put(base64_encode(child, kind == CaptureValue::UrlBase64));
				else put(child);
				if (kind == CaptureValue::SizedChild) put(" ");
				break;
			}
			case CaptureValue::NewLine: {
				const int indent = ReadCaptureValue<int>(values, cursor), count = ReadCaptureValue<int>(values, cursor);
				for (int i = 0; i < count; ++i) { put("\n"); if (indent > 0) put(std::string(static_cast<size_t>(indent), '\t')); }
				break;
			}
			case CaptureValue::Property: {
				const int indent = ReadCaptureValue<int>(values, cursor);
				put("\n"); put(std::string(static_cast<size_t>(indent), '\t')); put(ReadCaptureString(values, cursor)); put(" = ");
				break;
			}
			case CaptureValue::PeerBegin: ++peer; break;
			case CaptureValue::PeerEnd: --peer; break;
			case CaptureValue::PrimitiveBlock: {
				const auto decode = ReadCaptureValue<CheckpointBuffer::PrimitiveDecoder>(values, cursor);
				const auto block = ReadCaptureString(values, cursor);
				if (peer == 0) decode(text, block, false);
				break;
			}
			case CaptureValue::SizedRunBegin: sizedRuns.push_back(peer == 0 ? std::optional<size_t>(text.size()) : std::nullopt); break;
			case CaptureValue::SizedRunEnd: {
				if (sizedRuns.empty()) throw std::logic_error("unmatched owned sized run");
				const auto begin = sizedRuns.back(); sizedRuns.pop_back();
				if (begin) {
					std::string size; AppendCaptureNumber(size, text.size() - *begin); size.push_back(' ');
					text.insert(*begin, size); text.push_back(' ');
				}
				break;
			}
			default: throw std::logic_error("unknown owned checkpoint value");
		}
	}
	if (!sizedRuns.empty()) throw std::logic_error("open owned sized run");
	return text;
}

std::shared_future<void> CheckpointBuffer::PrepareCaptureStorage(size_t bytes) {
	static constexpr size_t chunk = 1 << 20;
	constexpr size_t ceiling = 256 << 20;
	std::lock_guard lock(s_ArenaPoolMutex);
	if (s_NextPrepared && !s_NextPrepared->ready.load(std::memory_order_acquire)) return s_NextPrepared->task;
	auto prepared = std::make_shared<CheckpointArenaGroup::Prepared>();
	const auto owner = std::this_thread::get_id();
	const size_t count = (std::min(bytes, ceiling) + chunk - 1) / chunk;
	prepared->task = prepared->completion.get_future().share();
	g_ThreadMan.GetBackgroundThreadPool().push_task([prepared, owner, count]() mutable {
		auto completion = std::move(prepared->completion);
		try {
			if (std::this_thread::get_id() == owner) throw std::logic_error("checkpoint preparation requires a worker");
			const auto started = std::chrono::steady_clock::now();
			const auto& helpers = g_ThreadMan.GetCheckpointThreadPool();
			const auto poolUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
			prepared->blocks.reserve(count);
			for (size_t index = 0; index < count; ++index) {
				auto block = std::make_shared<CheckpointArenaGroup::Block>(chunk, alignof(std::max_align_t));
				std::memset(block->address, 0, chunk);
				prepared->blocks.push_back(std::move(block));
			}
			prepared->ready.store(true, std::memory_order_release);
			System::PrintDiagnosticLine(std::format("[checkpoint-storage] worker=1 blocks={} bytes={} prepare_us={} helpers={} pool_us={}", count, count * chunk,
			    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count(), helpers.get_thread_count(), poolUs));
			prepared.reset();
			completion.set_value();
		} catch (...) {
			if (prepared) {
				{ std::lock_guard cleanup(prepared->mutex); prepared->blocks.clear(); }
				prepared->ready.store(true, std::memory_order_release);
				prepared.reset();
			}
			completion.set_exception(std::current_exception());
		}
	});
	s_NextPrepared = prepared;
	return prepared->task;
}

void CheckpointBuffer::CancelCaptureStorage() {
	std::lock_guard lock(s_ArenaPoolMutex);
	if (s_NextPrepared) ReleasePreparedStorage(std::move(s_NextPrepared));
}

void CheckpointBuffer::WaitForPreparedStorageRelease() {
	std::vector<std::shared_future<void>> tasks;
	{
		std::lock_guard lock(s_ArenaPoolMutex);
		tasks.swap(s_PreparedReleaseTasks);
	}
	for (auto& task: tasks) task.get();
}

CheckpointBuffer::ArenaPoolScope::ArenaPoolScope(bool enabled, bool releasePreparedOnWorker) {
	if (!enabled) return;
	std::lock_guard lock(s_ArenaPoolMutex);
	if (!s_ArenaPoolUsers) {
		s_ArenaPool = std::make_shared<CheckpointArenaPool>();
		s_ArenaPool->prepared = std::move(s_NextPrepared);
		s_ArenaPoolEpoch.store(++s_ArenaPoolNext, std::memory_order_release);
	}
	s_ArenaPool->releasePreparedOnWorker |= releasePreparedOnWorker;
	++s_ArenaPoolUsers;
	m_Entered = true;
	s_CheckpointPerformance.Enter();
}

CheckpointBuffer::ArenaPoolScope::~ArenaPoolScope() {
	if (!m_Entered) return;
	LeaveCheckpointPerformance performance;
	std::lock_guard lock(s_ArenaPoolMutex);
	if (--s_ArenaPoolUsers) return;
	s_ArenaPoolEpoch.store(0, std::memory_order_release);
	if (CaptureTrace::Active()) {
		std::lock_guard groups(s_ArenaPool->mutex);
		size_t blocks = 0, bytes = 0;
		for (const auto& [thread, group]: s_ArenaPool->groups) { blocks += group->Blocks(); bytes += group->Bytes(); }
		CaptureTrace::Span receipt("checkpoint_pool_allocations", std::format("threads={} blocks={} bytes={}", s_ArenaPool->groups.size(), blocks, bytes));
	}
	if (s_ArenaPool->releasePreparedOnWorker && s_ArenaPool->prepared) ReleasePreparedStorage(std::move(s_ArenaPool->prepared));
	s_ArenaPool.reset();
}

CheckpointBuffer::AllocationScope::AllocationScope(bool enabled) {
	if (enabled && !s_Arena) {
		s_Arena = std::shared_ptr<CheckpointArena>(new CheckpointArena, [](CheckpointArena* arena) { arena->ReleaseBlock(); });
		m_Entered = true;
		s_CheckpointPerformance.Enter();
	}
}

CheckpointBuffer::AllocationScope::~AllocationScope() {
	if (m_Entered) {
		LeaveCheckpointPerformance performance;
		if (CaptureTrace::Active()) {
			CaptureTrace::Span receipt("checkpoint_arena_allocations", std::format("blocks={} bytes={} backing_blocks={} backing_bytes={} nodes={} value_bytes={} value_capacity={} child_bytes={} child_capacity={}", s_Arena->receipts.blocks, s_Arena->receipts.bytes, s_Arena->group ? s_Arena->group->Blocks() : 0, s_Arena->group ? s_Arena->group->Bytes() : 0, s_Arena->nodes, s_Arena->valueBytes, s_Arena->valueCapacity, s_Arena->childBytes, s_Arena->childCapacity));
		}
		s_Arena.reset();
	}
}

std::shared_ptr<std::pmr::memory_resource> CheckpointBuffer::LeaseCaptureStorage() {
	if (!s_Arena) return {};
	return {s_Arena, s_Arena->storage};
}

std::shared_ptr<void> CheckpointBuffer::AllocateCaptureBytes(size_t bytes) {
	return AllocateCaptureBytes(bytes, alignof(uint8_t));
}

std::shared_ptr<void> CheckpointBuffer::AllocateCaptureBytes(size_t bytes, size_t alignment) {
	if (auto group = CurrentArenaGroup()) return group->AllocateBytes(bytes, alignment);
	auto* upstream = std::pmr::get_default_resource();
	void* address = upstream->allocate(bytes, alignment);
	return {address, [upstream, bytes, alignment](void* value) { upstream->deallocate(value, bytes, alignment); }};
}

CheckpointBuffer::CheckpointBuffer(bool reserve) : m_Arena(reserve ? s_Arena : nullptr),
	m_Values(m_Arena ? m_Arena->storage : std::pmr::get_default_resource()),
	m_Children(m_Arena ? m_Arena->storage : std::pmr::get_default_resource()),
	m_OwnedBlocks(m_Arena ? m_Arena->storage : std::pmr::get_default_resource()) {}

char* CheckpointBuffer::ReserveValues(size_t size) {
	if (!m_Arena) {
		const size_t offset = m_Values.size();
		if (size > m_Values.max_size() - offset) throw std::length_error("checkpoint field block is too large");
		m_Values.resize(offset + size);
		return m_Values.data() + offset;
	}
	if (size > std::numeric_limits<size_t>::max() - m_ValueSize) throw std::length_error("checkpoint field block is too large");
	if (!m_LastValues || size > m_LastValues->capacity - m_LastValues->size) {
		const size_t capacity = std::max(size, m_LastValues ? std::min(size_t{2048}, m_LastValues->capacity) * 2 : size_t{64});
		if (capacity > std::numeric_limits<size_t>::max() - sizeof(ValueChunk) || capacity > std::numeric_limits<size_t>::max() - m_ValueCapacity) throw std::length_error("checkpoint field block is too large");
		void* address = m_Arena->storage->allocate(sizeof(ValueChunk) + capacity, alignof(ValueChunk));
		auto* chunk = std::construct_at(static_cast<ValueChunk*>(address), capacity);
		if (m_LastValues) m_LastValues->next = chunk; else m_FirstValues = chunk;
		m_LastValues = chunk;
		m_ValueCapacity += capacity;
	}
	char* into = m_LastValues->Bytes() + m_LastValues->size;
	m_LastValues->size += size;
	m_ValueSize += size;
	return into;
}

void CheckpointBuffer::AppendValues(std::string_view values) {
	if (!m_Arena) m_Values.append(values);
	else if (!values.empty()) std::memcpy(ReserveValues(values.size()), values.data(), values.size());
}

void CheckpointBuffer::Raw(std::string_view text) { Copy(CaptureValue::Raw, static_cast<uint64_t>(text.size())); AppendValues(text); }
void CheckpointBuffer::Integer(int64_t value, bool space) { Copy(space ? CaptureValue::SpacedInteger : CaptureValue::Integer, value); }
void CheckpointBuffer::Unsigned(uint64_t value, bool space) { Copy(space ? CaptureValue::SpacedUnsigned : CaptureValue::Unsigned, value); }
void CheckpointBuffer::Real(float value) { Copy(CaptureValue::Float, value); }
void CheckpointBuffer::Real(double value) { Copy(CaptureValue::Double, value); }
void CheckpointBuffer::ElapsedSimTime(int64_t startTicks, double ticksPerMS) {
	Copy(CaptureValue::ElapsedSimTime, startTicks, ticksPerMS);
	m_UsesSimTime = true;
	m_SimTimeTicks = g_TimerMan.GetSimTickCount();
}
void CheckpointBuffer::String(std::string_view value) { Copy(CaptureValue::String, static_cast<uint64_t>(value.size())); AppendValues(value); }
void CheckpointBuffer::Child(const CheckpointText& value, bool sized) { Copy(sized ? CaptureValue::SizedChild : CaptureValue::Child, static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::Base64(const CheckpointText& value, bool url) { Copy(url ? CaptureValue::UrlBase64 : CaptureValue::Base64, static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::GraphString(const CheckpointText& value) { Copy(CaptureValue::GraphString, static_cast<uint64_t>(m_Children.size())); m_Children.push_back(value); }
void CheckpointBuffer::NewLine(int indent, int count) { Copy(CaptureValue::NewLine, indent, count); }
void CheckpointBuffer::Property(std::string_view name, int indent) { Copy(CaptureValue::Property, indent, static_cast<uint64_t>(name.size())); AppendValues(name); }
void CheckpointBuffer::PeerBegin() { Copy(CaptureValue::PeerBegin); m_HasPeer = true; }
void CheckpointBuffer::PeerEnd() { Copy(CaptureValue::PeerEnd); }
void CheckpointBuffer::PrimitiveBlock(std::string_view values, PrimitiveDecoder decoder) {
	Copy(CaptureValue::PrimitiveBlock, decoder, static_cast<uint64_t>(values.size()));
	AppendValues(values);
	m_HasPrimitiveBlocks = true;
}

void CheckpointBuffer::OwnedPrimitiveBlock(std::shared_ptr<const void> storage, PrimitiveDecoder decoder, size_t ownedBytes) {
	const void* address = storage.get();
	if (!address) throw std::logic_error("owned primitive block has no storage");
	m_OwnedBlocks.push_back(std::move(storage));
	PrimitiveBlock({reinterpret_cast<const char*>(&address), sizeof(address)}, decoder);
	m_OwnedBlockBytes += ownedBytes;
}

void CheckpointBuffer::SizedRunBegin() { Copy(CaptureValue::SizedRunBegin); }
void CheckpointBuffer::SizedRunEnd() { Copy(CaptureValue::SizedRunEnd); }

CheckpointText CheckpointBuffer::Finish() {
	if (m_Arena && CaptureTrace::Active()) {
		++m_Arena->nodes;
		m_Arena->valueBytes += m_ValueSize;
		m_Arena->valueCapacity += m_ValueCapacity;
		m_Arena->childBytes += m_Children.size() * sizeof(CheckpointText);
		m_Arena->childCapacity += m_Children.capacity() * sizeof(CheckpointText);
	}
	std::shared_ptr<CheckpointText::Data> data;
	if (m_Arena) {
		if (m_Arena->group) data = std::allocate_shared<CheckpointText::Data>(CheckpointAllocator<CheckpointText::Data>(m_Arena.get()), m_Arena->storage);
		else data = std::allocate_shared<CheckpointText::Data::Legacy>(CheckpointAllocator<CheckpointText::Data::Legacy>(m_Arena.get()), m_Arena->storage);
	} else data = CheckpointText::Data::Create();
	data->values = std::move(m_Values);
	data->chunks = std::exchange(m_FirstValues, nullptr);
	data->valueSize = std::exchange(m_ValueSize, 0);
	m_LastValues = nullptr;
	m_ValueCapacity = 0;
	data->children = std::move(m_Children);
	data->ownedBlocks = std::move(m_OwnedBlocks);
	data->ownedBytes = (data->chunks ? data->valueSize : data->values.size()) + std::exchange(m_OwnedBlockBytes, 0);
	data->hasPeer = m_HasPeer;
	data->hasPrimitiveBlocks = m_HasPrimitiveBlocks;
	data->usesSimTime = m_UsesSimTime;
	data->simTimeTicks = m_SimTimeTicks;
	for (const auto& child: data->children) {
		data->ownedBytes += child.OwnedBytes();
		data->hasPeer = data->hasPeer || (child.m_Data && child.m_Data->hasPeer);
		if (!data->usesSimTime && child.m_Data && child.m_Data->usesSimTime) {
			data->usesSimTime = true;
			data->simTimeTicks = child.m_Data->simTimeTicks;
		}
	}
	return CheckpointText(std::move(data));
}

CheckpointCache::CheckpointCache(const CheckpointCache& other) :
	m_Transient(other.m_Transient), m_Entries(other.m_Entries), m_Presets(other.m_Presets), m_Retired(other.m_Retired),
	m_Generation(other.m_Generation), m_Touched(other.m_Touched), m_Reused(other.m_Reused), m_Pixels(other.m_Pixels) {
	if (other.m_CapturedEntries) {
		m_CapturedEntries = std::make_unique<CapturedEntries>(std::shared_ptr<std::pmr::memory_resource>{});
		m_CapturedEntries->values = other.m_CapturedEntries->values;
	}
}

CheckpointCache& CheckpointCache::operator=(const CheckpointCache& other) {
	if (this != &other) *this = CheckpointCache(other);
	return *this;
}

const Entity* CheckpointCache::FindPreset(const std::string& type, const std::string& name, int module) {
	const bool batch = CheckpointWriter::BatchEnabled();
	if (batch && (module < 0 || name.find('/') != std::string::npos)) return g_PresetMan.GetEntityPreset(type, name, module);
	const auto key = std::tuple(module, std::string_view(type), std::string_view(name));
	if (const auto found = m_Presets.find(key); found != m_Presets.end() && (!batch || (found->second && found->second->GetModuleID() == module))) return found->second;
	const Entity* preset = batch ? BitmapPixelCaptureScope::FindPreset(type, name, module) : g_PresetMan.GetEntityPreset(type, name, module);
	if (!batch || (preset && preset->GetModuleID() == module)) m_Presets.insert_or_assign(std::tuple(module, type, name), preset);
	return preset;
}

CheckpointText CheckpointCache::Remember(const void* owner, unsigned channel, CheckpointText value) {
	if (channel != 16) return Remember(owner, channel, std::move(value), 0);
	const auto start = std::chrono::steady_clock::now();
	CheckpointText result = Remember(owner, channel, std::move(value), 0);
	CheckpointGraphIndex::Get().NoteWalkPart(nullptr, "cache", 0, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count(), false, {});
	return result;
}

CheckpointText CheckpointCache::Remember(const void* owner, unsigned channel, CheckpointText value, uint64_t stamp, uint64_t identity, const MovableObject* object) {
	Entry& entry = CaptureEntry(owner, channel);
	++m_Touched;
	entry.generation = m_Generation;
	entry.stamp = stamp;
	entry.identity = identity;
	entry.object = object;
	if (entry.text.SameValues(value)) { ++m_Reused; m_Retired.push_back(std::move(value)); return entry.text; }
	m_Retired.push_back(value);
	value = value.ReuseChildren(entry.text);
	m_Retired.push_back(std::move(entry.text));
	entry.text = std::move(value);
	return entry.text;
}

CheckpointCache::Entry& CheckpointCache::CaptureEntry(const void* owner, unsigned channel) {
	if (!m_CapturedEntries && m_Transient && CheckpointWriter::BatchEnabled() && m_Entries.empty()) m_CapturedEntries = std::make_unique<CapturedEntries>();
	return m_CapturedEntries ? m_CapturedEntries->values[{owner, channel}] : m_Entries[owner][channel];
}

const CheckpointCache::Entry* CheckpointCache::FindEntry(const void* owner, unsigned channel) const {
	if (m_CapturedEntries) {
		const auto found = m_CapturedEntries->values.find({owner, channel});
		return found == m_CapturedEntries->values.end() ? nullptr : &found->second;
	}
	const auto owners = m_Entries.find(owner);
	if (owners == m_Entries.end()) return nullptr;
	const auto entry = owners->second.find(channel);
	if (entry == owners->second.end()) return nullptr;
	return &entry->second;
}

const CheckpointText* CheckpointCache::Peek(const void* owner, unsigned channel) const {
	const Entry* entry = FindEntry(owner, channel);
	return entry ? &entry->text : nullptr;
}

const CheckpointText* CheckpointCache::PeekCurrent(const void* owner, unsigned channel) const {
	const Entry* entry = FindEntry(owner, channel);
	return entry && entry->generation == m_Generation ? &entry->text : nullptr;
}

uint64_t CheckpointCache::Identity(const void* owner, unsigned channel) const {
	const Entry* entry = FindEntry(owner, channel);
	return entry && entry->object.get() ? entry->identity : 0;
}

uint64_t CheckpointCache::Stamp(const void* owner, unsigned channel) const {
	const Entry* entry = FindEntry(owner, channel);
	return entry ? entry->stamp : 0;
}

bool CheckpointCache::Touch(const void* owner, unsigned channel) {
	Entry* entry = const_cast<Entry*>(FindEntry(owner, channel));
	if (!entry) return false;
	entry->generation = m_Generation;
	++m_Touched;
	++m_Reused;
	return true;
}

CheckpointText CheckpointCache::CapturePixels(const BITMAP* bitmap) {
	if (!bitmap) return CheckpointText(std::string());
	Pixels& previous = m_Pixels[bitmap];
	previous.generation = m_Generation;
	if (const auto shared = BitmapPixelCaptureScope::Capture(bitmap, previous.snapshot)) {
		m_Retired.push_back(std::move(previous.text));
		previous.snapshot = shared->first;
		previous.text = shared->second;
		return previous.text;
	}
	auto snapshot = BitmapSnapshot::Capture(bitmap, previous.snapshot);
	if (previous.snapshot && snapshot->SamePixels(*previous.snapshot)) return previous.text;
	CheckpointText text = CheckpointText::Deferred([snapshot] { return snapshot->PixelBytes(); }, snapshot->LogicalBytes());
	m_Retired.push_back(std::move(previous.text));
	previous.snapshot = std::move(snapshot);
	previous.text = std::move(text);
	return previous.text;
}

struct BitmapPixelCaptureScope::State {
	bool deferRows = false;
	struct Cell { std::once_flag once; std::shared_ptr<const BitmapSnapshot> snapshot; CheckpointText text; };
	struct Cells {
		std::mutex mutex;
		std::optional<std::unordered_map<const BITMAP*, std::shared_ptr<Cell>>> values;
	};
	struct Presets {
		std::mutex mutex;
		std::optional<std::map<std::tuple<int, std::string, std::string>, const Entity*, std::less<>>> values;
	};
	std::array<Presets, 64> presets;
	std::atomic<size_t> presetQueries{0};
	std::array<Cells, 64> pixelShards;
	std::mutex mutex;
	std::unordered_map<const BITMAP*, std::shared_ptr<Cell>> cells;
	std::unordered_map<const CheckpointPagePool::Allocation*, std::shared_ptr<const CheckpointPagePool::Snapshot>> pageCopies;
	std::thread::id captureThread = std::this_thread::get_id();
	~State() {
		static const bool report = [] { const char* value = std::getenv("CCCP_CHECKPOINT_PHASES"); return value && std::string_view(value) == "1"; }();
		const auto started = report ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		size_t count = cells.size();
		cells.clear();
		for (auto& shard: pixelShards) if (shard.values) { count += shard.values->size(); shard.values->clear(); }
		if (report) System::PrintDiagnosticLine(std::format("[checkpoint-pixel-release] entries={} capture_thread={} release_thread={} off_capture_thread={} us={}", count,
			std::hash<std::thread::id>{}(captureThread), std::hash<std::thread::id>{}(std::this_thread::get_id()), captureThread != std::this_thread::get_id(),
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
	}
};
std::atomic<BitmapPixelCaptureScope::State*> BitmapPixelCaptureScope::s_Current{nullptr};
BitmapPixelCaptureScope::BitmapPixelCaptureScope(bool deferRows) : m_State(std::make_unique<State>()) {
	m_State->deferRows = deferRows;
	m_Previous = s_Current.exchange(m_State.get());
}
BitmapPixelCaptureScope::~BitmapPixelCaptureScope() { if (m_State) s_Current.store(m_Previous); }
std::shared_ptr<const void> BitmapPixelCaptureScope::TakeStorage() {
	if (!m_State || s_Current.load() != m_State.get()) throw std::logic_error("pixel storage requires the current joined capture");
	s_Current.store(m_Previous);
	return std::shared_ptr<State>(std::move(m_State));
}

std::shared_ptr<const CheckpointPagePool::Snapshot> BitmapPixelCaptureScope::FreezePages(const std::shared_ptr<CheckpointPagePool::Allocation>& allocation) {
	State* state = s_Current.load();
	if (!state) return allocation->Freeze();
	std::lock_guard lock(state->mutex);
	auto& snapshot = state->pageCopies[allocation.get()];
	if (!snapshot) snapshot = allocation->Freeze();
	return snapshot;
}
const Entity* BitmapPixelCaptureScope::FindPreset(const std::string& type, const std::string& name, int module) {
	State* state = s_Current.load();
	if (!state || module < 0 || name.find('/') != std::string::npos) return g_PresetMan.GetEntityPreset(type, name, module);
	const size_t hash = std::hash<std::string_view>{}(type) ^ (std::hash<std::string_view>{}(name) << 1) ^ std::hash<int>{}(module);
	auto& shard = state->presets[hash % state->presets.size()];
	std::lock_guard lock(shard.mutex);
	if (!shard.values) shard.values.emplace();
	const auto key = std::tuple(module, std::string_view(type), std::string_view(name));
	if (const auto found = shard.values->find(key); found != shard.values->end() && found->second->GetModuleID() == module) return found->second;
	const Entity* preset = g_PresetMan.GetEntityPreset(type, name, module);
	// Mods can add a missing local preset; only existing local matches are stable.
	if (preset && preset->GetModuleID() == module) shard.values->insert_or_assign(std::tuple(module, type, name), preset);
	state->presetQueries.fetch_add(1, std::memory_order_relaxed);
	return preset;
}
std::optional<std::pair<std::shared_ptr<const BitmapSnapshot>, CheckpointText>> BitmapPixelCaptureScope::Capture(
    const BITMAP* bitmap, const std::shared_ptr<const BitmapSnapshot>& previous) {
	if (const auto* snapshot = CheckpointNativeSnapshot::Current()) if (auto pixels = snapshot->Pixels(bitmap)) return pixels;
	State* state = s_Current.load();
	if (!state) return std::nullopt;
	std::shared_ptr<State::Cell> cell;
	if (CheckpointWriter::BatchEnabled()) {
		uintptr_t hash = reinterpret_cast<uintptr_t>(bitmap) >> 4;
		hash ^= hash >> 16;
		auto& shard = state->pixelShards[hash % state->pixelShards.size()];
		std::lock_guard lock(shard.mutex);
		if (!shard.values) shard.values.emplace();
		auto& entry = (*shard.values)[bitmap];
		if (!entry) entry = std::make_shared<State::Cell>();
		cell = entry;
	} else {
		std::lock_guard lock(state->mutex);
		auto& entry = state->cells[bitmap];
		if (!entry) entry = std::make_shared<State::Cell>();
		cell = entry;
	}
	std::call_once(cell->once, [&] {
		cell->snapshot = state->deferRows ? BitmapSnapshot::Freeze(bitmap, previous) : BitmapSnapshot::Capture(bitmap, previous);
		cell->text = CheckpointText::Deferred([snapshot = cell->snapshot] { return snapshot->PixelBytes(); }, cell->snapshot->LogicalBytes());
	});
	return std::pair{cell->snapshot, cell->text};
}

size_t CheckpointCache::PixelBytes() const {
	size_t bytes = 0;
	for (const auto& [bitmap, pixels]: m_Pixels) if (pixels.snapshot) bytes += pixels.snapshot->OwnedBytes();
	return bytes;
}

std::string CheckpointCache::Census() const {
	size_t entries = 0, bytes = 0, retiredBytes = 0;
	for (const auto& [owner, channels]: m_Entries) {
		entries += channels.size();
		for (const auto& [channel, entry]: channels) bytes += entry.text.OwnedBytes();
	}
	for (const CheckpointText& text: m_Retired) retiredBytes += text.OwnedBytes();
	// Which kind of owner holds the entries: per channel, and how many belong to a world object.
	std::map<unsigned, size_t> perChannel;
	size_t withObject = 0;
	for (const auto& [owner, channels]: m_Entries) {
		for (const auto& [channel, entry]: channels) {
			++perChannel[channel];
			if (entry.identity != 0) ++withObject;
		}
	}
	std::string channelText;
	std::unordered_set<const void*> capturedOwners;
	if (m_CapturedEntries) {
		for (const auto& [key, entry]: m_CapturedEntries->values) {
			++entries; bytes += entry.text.OwnedBytes(); ++perChannel[key.second];
			if (entry.identity != 0) ++withObject;
			capturedOwners.insert(key.first);
		}
	}
	for (const auto& [channel, count]: perChannel) channelText += std::format("{}{}:{}", channelText.empty() ? "" : ",", channel, count);
	return std::format("entries={} entry_mb={} pixels={} retired={} retired_mb={} owners={} with_identity={} channels={}", entries, bytes >> 20, m_Pixels.size(), m_Retired.size(),
	                   retiredBytes >> 20, m_Entries.size() + capturedOwners.size(), withObject, channelText.empty() ? "none" : channelText);
}

std::vector<CheckpointText> CheckpointCache::RetireUnused() {
	for (auto pixels = m_Pixels.begin(); pixels != m_Pixels.end();) {
		if (pixels->second.generation != m_Generation) {
			m_Retired.push_back(std::move(pixels->second.text));
			pixels = m_Pixels.erase(pixels);
		} else ++pixels;
	}
	for (auto owners = m_Entries.begin(); owners != m_Entries.end();) {
		for (auto entry = owners->second.begin(); entry != owners->second.end();) {
			if (entry->second.generation != m_Generation) {
				m_Retired.push_back(std::move(entry->second.text));
				entry = owners->second.erase(entry);
			} else ++entry;
		}
		if (owners->second.empty()) owners = m_Entries.erase(owners); else ++owners;
	}
	if (m_CapturedEntries) {
		for (auto entry = m_CapturedEntries->values.begin(); entry != m_CapturedEntries->values.end();) {
			if (entry->second.generation != m_Generation) {
				m_Retired.push_back(std::move(entry->second.text));
				entry = m_CapturedEntries->values.erase(entry);
			} else ++entry;
		}
	}
	return std::exchange(m_Retired, {});
}

CheckpointText Writer::Capture(const std::function<void(Writer&)>& visit, int indent) {
	return CheckpointWriter::CaptureValues([&] {
		CheckpointBuffer buffer;
		Writer writer;
		writer.m_Capture = &buffer;
		writer.m_IndentCount = indent;
		writer.m_Snapshot = true;
		visit(writer);
		return buffer.Finish();
	});
}

void Writer::Append(const CheckpointText& text) {
	if (m_Capture) m_Capture->Child(text); else *m_Stream << text.Text();
}

void Writer::ElapsedSimTime(const Timer& timer) {
	if (m_Capture) m_Capture->ElapsedSimTime(timer.GetStartSimTimeMS(), static_cast<double>(g_TimerMan.GetTicksPerSecond()) * 0.001);
	else *this << timer.GetElapsedSimTimeMS();
}

void Writer::Clear() {
	m_Stream = nullptr;
	m_FilePath.clear();
	m_FolderPath.clear();
	m_FileName.clear();
	m_IndentCount = 0;
	m_Snapshot = false;
	m_Capture = nullptr;
	m_SaveOverrides = nullptr;
	m_CaptureObject = nullptr;
}

Writer::Writer(const std::string& fileName, bool append, bool createDir) {
	Clear();
	Create(fileName, append, createDir);
}

Writer::Writer(std::unique_ptr<std::ostream>&& stream) {
	Clear();
	Create(std::move(stream));
}

int Writer::Create(const std::string& fileName, bool append, bool createDir) {
	m_FilePath = fileName;

	// Extract filename and folder path
	size_t slashPos = m_FilePath.find_last_of("/\\");
	m_FileName = m_FilePath.substr(slashPos + 1);
	m_FolderPath = m_FilePath.substr(0, slashPos + 1);

	if (createDir && !std::filesystem::exists(System::GetWorkingDirectory() + m_FolderPath)) {
		System::MakeDirectory(System::GetWorkingDirectory() + m_FolderPath);
	}

	auto ofStream = std::make_unique<std::ofstream>(fileName, append ? (std::ios::out | std::ios::app | std::ios::ate) : (std::ios::out | std::ios::trunc));
	*ofStream << std::fixed << std::setprecision(6);
	return Create(std::move(ofStream));
}

int Writer::Create(std::unique_ptr<std::ostream>&& stream) {
	m_Stream = std::move(stream);
	// Integer grouping is part of a stream's locale too; saved text never uses it.
	m_Stream->imbue(std::locale::classic());
	if (!m_Stream->good()) {
		return -1;
	}

	return 0;
}

void Writer::NewLine(bool toIndent, int lineCount) const {
	if (m_Capture) { m_Capture->NewLine(toIndent ? m_IndentCount : 0, lineCount); return; }
	for (int lines = 0; lines < lineCount; ++lines) {
		*m_Stream << "\n";
		if (toIndent) {
			*m_Stream << std::string(m_IndentCount, '\t');
		}
	}
}

bool RTE::RunOwnedCheckpointSelfTest() {
	bool passed = true;
	const auto check = [&passed](bool result, const char* name, const std::string& detail = {}) {
		std::cout << "[script-graph-selftest] " << (result ? "PASS" : "FAIL") << " " << name;
		if (!result && !detail.empty()) std::cout << " " << detail;
		std::cout << std::endl;
		passed = result && passed;
	};
	try {
		const std::string pageMismatch = CheckpointPagePool::SelfTestMismatch();
		check(pageMismatch.empty(), "native_checkpoint_pages_preserve_overlapping_generations_and_outlive_the_pool", pageMismatch);
#ifdef _WIN32
		{
			auto& performance = s_CheckpointPerformance;
			CheckpointThreadPerformance::Policy before;
			if (performance.get && performance.set && performance.get(GetCurrentThread(), 3, &before, sizeof(before))) {
				struct RestorePolicy {
					CheckpointThreadPerformance& performance;
					CheckpointThreadPerformance::Policy policy;
					~RestorePolicy() { performance.set(GetCurrentThread(), 3, &policy, sizeof(policy)); }
				} restore{performance, before};
				CheckpointThreadPerformance::Policy eco{1, 1, 1};
				bool exact = performance.set(GetCurrentThread(), 3, &eco, sizeof(eco)) != FALSE;
				const auto matches = [&](ULONG control, ULONG state) {
					CheckpointThreadPerformance::Policy current;
					return performance.get(GetCurrentThread(), 3, &current, sizeof(current)) && current.control == control && current.state == state;
				};
				{
					CheckpointBuffer::ArenaPoolScope pool(false);
					CheckpointBuffer::AllocationScope allocation(false);
					exact = exact && matches(1, 1);
				}
				{
					CheckpointBuffer::ArenaPoolScope pool(true);
					exact = exact && matches(1, 0);
					{
						CheckpointBuffer::AllocationScope allocation(true);
						CheckpointBuffer::AllocationScope nested(true);
						exact = exact && matches(1, 0);
					}
					exact = exact && matches(1, 0);
				}
				exact = exact && matches(1, 1) && performance.depth == 0;
				check(exact, "checkpoint_scopes_restore_thread_power_policy");
			} else {
				std::cout << "[script-graph-selftest] SKIP checkpoint_scopes_restore_thread_power_policy unsupported" << std::endl;
			}
		}
#endif
		int value = 17;
		std::string binary("x\0y", 3);
		const auto save = [&]() -> std::string { CheckpointWriter writer("Owned1"); writer(value, binary); return writer.Text(); };
		const std::string reference = save();
		const CheckpointText captured = CheckpointWriter::CaptureNative(save);
		value = 91; binary.assign("changed");
		check(captured.Text() == reference, "owned_checkpoint_copies_native_values");
		{
			CheckpointText values = Writer::Capture([](Writer& writer) {
				writer.NewPropertyWithValue("Binary", std::string("x\0y\xff", 4));
				writer.PerPeerBegin(); writer.NewPropertyWithValue("Seat", 19); writer.PerPeerEnd();
			});
			const std::string full = values.Text(), shared = values.SharedText();
			const std::weak_ptr<CheckpointText::Data> fields = values.m_Data;
			const CheckpointText compact = values.Compact();
			values = {};
			check(fields.expired() && compact.Text() == full && compact.SharedText() == shared,
			      "completed_checkpoint_releases_fields_and_keeps_full_and_shared_bytes");
		}
		{
			const auto record = [](int indent) {
				CheckpointWriter::BatchOverride ordinary(false);
				return Writer::Capture([](Writer& writer) {
					writer.ObjectStart("FrozenMenuProbe");
					writer.NewPropertyWithValue("Literal", std::string("line\n\tlooks = like a property\0tail", 34));
					writer.PerPeerBegin(); writer.NewPropertyWithValue("Peer", 17); writer.PerPeerEnd();
					writer.NewPropertyWithValue("Child", Writer::Capture([](Writer& child) { child.NewPropertyWithValue("UnindentedNative", "same"); }));
					writer.ObjectEnd();
				}, indent);
			};
			const auto baseline = record(1), deeper = record(5);
			const auto relocated = baseline.ReindentWriter(4);
			check(relocated.Text() == deeper.Text() && relocated.SharedText() == deeper.SharedText(), "frozen_writer_indent_preserves_literal_bytes_and_peer_runs");
			int calls = 0;
			const auto deferred = CheckpointText::DeferredWriter([&] { ++calls; return record(1); });
			const auto late = deferred.ReindentWriter(2).ReindentWriter(2);
			const bool lazy = calls == 0;
			const auto written = std::async(std::launch::async, [late] { return std::pair{late.Text(), late.SharedText()}; }).get();
			check(lazy && calls == 1 && written.first == deeper.Text() && written.second == deeper.SharedText() &&
			      deferred.Text() == baseline.Text() && deferred.ReindentWriter(4).Text() == deeper.Text() && calls == 1,
			      "deferred_writer_relocates_owned_properties_once_without_rewriting_literal_bytes");
		}
		{
			std::vector<int> source{7, -19, 31}, values;
			std::deque<Vector> vectors{Vector(-0.0F, 13.5F), Vector(17.25F, -23.5F)}, copiedVectors;
			std::string bytes("line\n\tvalue\0\xff", 13), copiedBytes;
			const auto expected = source;
			const auto expectedVectors = vectors;
			const auto expectedBytes = bytes;
			CheckpointNativeSnapshot snapshot;
			values = snapshot.Freeze(source, &values);
			copiedVectors = snapshot.Freeze(vectors, &copiedVectors);
			copiedBytes = snapshot.Freeze(bytes, &copiedBytes);
			bool earlyRefused = false, lateRefused = false, partialRefused = false;
			try { CheckpointNativeSnapshot::ReadScope early(&snapshot); } catch (const std::logic_error&) { earlyRefused = true; }
			source.assign(1, 99); vectors.clear(); bytes.assign("changed");
			snapshot.SealBoundary();
			try {
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
				CheckpointNativeSnapshot::ReadScope failed(&snapshot);
			} catch (const std::bad_alloc&) { lateRefused = true; }
			const bool unexpanded = values.empty() && copiedVectors.empty() && copiedBytes.empty();
			try {
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects, 1);
				CheckpointNativeSnapshot::ReadScope failed(&snapshot);
			} catch (const std::bad_alloc&) { partialRefused = true; }
			const bool retried = std::async(std::launch::async, [&] {
				CheckpointNativeSnapshot::ReadScope read(&snapshot);
				bool exact = values == expected && copiedBytes == expectedBytes && copiedVectors.size() == expectedVectors.size();
				for (size_t index = 0; exact && index < copiedVectors.size(); ++index) exact =
				    std::bit_cast<uint32_t>(copiedVectors[index].m_X) == std::bit_cast<uint32_t>(expectedVectors[index].m_X) &&
				    std::bit_cast<uint32_t>(copiedVectors[index].m_Y) == std::bit_cast<uint32_t>(expectedVectors[index].m_Y);
				return exact;
			}).get();
			check(earlyRefused && lateRefused && partialRefused && unexpanded && retried, "boundary_sequences_expand_from_owned_bytes_after_mutation_and_failed_worker_retry");
		}
		{
			CheckpointText frozen;
			std::string full, shared;
			{
				std::vector<std::pair<Vector, float>> vectors{{Vector(-0.0F, 7.25F), std::bit_cast<float>(uint32_t{0x7fc01234})}, {Vector(9, -13), -0.0F}};
				std::list<std::array<int, 3>> integers{{-7, 0, 19}, {31, -43, 59}};
				std::deque<std::string> strings{std::string("x\0y\xff", 4), "", "last"};
				std::set<std::string> names{"z", std::string("a\0b", 3), "a"};
				std::map<long, std::vector<std::pair<unsigned, double>>> nested{{-3, {{7, -0.0}, {9, 1.25}}}, {11, {}}};
				const auto saveCollections = [&] {
					CheckpointWriter writer("OwnedCollections1");
					writer(vectors, integers, strings, names);
					writer.PerPeer(nested);
					return writer.Text();
				};
				const auto ordinary = CheckpointWriter::CaptureNative(saveCollections);
				full = ordinary.Text(); shared = ordinary.SharedText();
				{
					CheckpointWriter::BatchScope batch(true);
					frozen = CheckpointWriter::CaptureNative(saveCollections);
				}
				vectors.clear(); integers.clear(); strings.clear(); names.clear(); nested.clear();
			}
			std::array<std::future<bool>, 4> readers;
			for (auto& reader: readers) reader = std::async(std::launch::async, [frozen, &full, &shared] { return frozen.Text() == full && frozen.SharedText() == shared; });
			bool exact = true;
			for (auto& reader: readers) exact = reader.get() && exact;
			check(exact, "owned_native_collections_pack_after_source_death_and_preserve_full_and_shared_bytes");
		}
		{
			CheckpointText frozen;
			std::string full, shared;
			{
				std::unordered_map<std::string, float> numbers{{"z", -0.0F}, {std::string("a\0b", 3), std::bit_cast<float>(uint32_t{0x7fc01234})}, {"a", 1.25F}};
				std::unordered_map<uint64_t, int64_t> integers{{std::numeric_limits<uint64_t>::max(), std::numeric_limits<int64_t>::min()}, {0, 17}, {31, -7}};
				std::unordered_map<int, std::string> peers{{9, std::string("x\0y\xff", 4)}, {-7, "first"}, {0, ""}};
				std::unordered_map<std::string, int> empty;
				const auto saveMaps = [&] {
					CheckpointWriter writer("OwnedMaps1");
					writer(numbers, integers, empty);
					writer.PerPeer(peers);
					return writer.Text();
				};
				const CheckpointText ordinary = CheckpointWriter::CaptureNative(saveMaps);
				full = ordinary.Text(); shared = ordinary.SharedText();
				{
					CheckpointWriter::BatchScope batch(true);
					frozen = CheckpointWriter::CaptureNative(saveMaps);
				}
				numbers.clear(); integers.clear(); peers.clear(); empty.emplace("changed", 77);
			}
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(actual.first == full && actual.second == shared, "owned_unordered_maps_sort_after_source_death_and_keep_full_and_shared_bytes");
		}
		{
			int64_t integer = std::numeric_limits<int64_t>::min();
			bool flag = true;
			float scalar = -0.0F;
			Vector vector(-0.0F, std::bit_cast<float>(uint32_t{0x7fc01234}));
			std::string binary("named\0field\xff", 12);
			const auto write = [&](Writer& writer) {
				WriteCapturedProperties(writer, CheckpointProperty<"Integer">(integer), CheckpointProperty<"Flag">(flag),
				    CheckpointProperty<"Scalar">(scalar), CheckpointProperty<"Vector">(vector), CheckpointProperty<"Binary">(binary));
				writer.PerPeerBegin();
				WriteCapturedProperties(writer, CheckpointProperty<"PeerInteger">(integer), CheckpointProperty<"PeerFlag">(flag));
				writer.PerPeerEnd();
			};
			const CheckpointText ordinary = Writer::Capture(write, 2);
			CheckpointText frozen;
			{
				CheckpointWriter::BatchScope batch(true);
				frozen = Writer::Capture(write, 2);
			}
			integer = 99; flag = false; scalar = 1.25F; vector.SetXY(33, 37); binary.assign("changed");
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(actual.first == ordinary.Text() && actual.second == ordinary.SharedText(), "owned_named_properties_preserve_full_and_shared_bytes_after_mutation");
		}
		{
			CheckpointText frozen;
			std::string full, shared;
			{
				std::vector<Vector> offsets{Vector(-0.0F, std::bit_cast<float>(uint32_t{0x7fc01234})), Vector(11.25F, -17.5F)};
				std::vector<int64_t> ids{std::numeric_limits<int64_t>::min(), 0, std::numeric_limits<int64_t>::max()};
				std::vector<std::string> binary{std::string("seq\0value\xff", 10), "", "repeated"};
				const auto write = [&](Writer& writer) {
					WriteCapturedPropertySequence<"Offsets">(writer, offsets);
					WriteCapturedPropertySequence<"IDs">(writer, ids);
					WriteCapturedPropertySequence<"Empty">(writer, std::vector<int>{});
					writer.PerPeerBegin();
					WriteCapturedPropertySequence<"Binary">(writer, binary);
					writer.PerPeerEnd();
				};
				const auto ordinary = Writer::Capture(write, 3);
				full = ordinary.Text(); shared = ordinary.SharedText();
				{
					CheckpointWriter::BatchScope batches(true);
					frozen = Writer::Capture(write, 3);
				}
			}
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(actual.first == full && actual.second == shared, "owned_property_sequences_outlive_sources_and_preserve_full_and_shared_bytes");
		}
		{
			CheckpointText frozen;
			std::string expected;
			{
				AtomGroup group;
				for (int index = 0; index < 3; ++index) {
					auto atom = std::make_unique<Atom>();
					atom->SetOffset(Vector(index == 0 ? -0.0F : float(index), -float(index + 7)));
					atom->SetPrevError(index * 31 - 7);
					atom->SetChangedDir(index != 1);
					group.AddAtom(atom.release(), index == 1 ? 13 : 7);
				}
				expected = Writer::Capture([&](Writer& writer) {
					WriteCapturedPropertySequence<"AtomGroupResidue">(writer, group.GetTravelResidue());
					WriteCapturedPropertySequence<"AtomGroupOffset">(writer, group.GetAtomOffsets());
					WriteCapturedPropertySequence<"AtomGroupSubID">(writer, group.GetAtomSubIDs());
					WriteCapturedPropertySequence<"AtomGroupMaterial">(writer, group.GetAtomMaterialIndices());
				}, 3).Text();
				CheckpointWriter::BatchScope batch(true);
				frozen = Writer::Capture([&](Writer& writer) { group.CaptureSnapshotProperties(writer); }, 3);
				for (Atom* atom: group.GetAtomList()) { atom->SetOffset(Vector(99, 101)); atom->SetPrevError(127); }
			}
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(actual.first == expected && actual.second == expected, "owned_atom_group_columns_preserve_order_and_outlive_mutated_sources");
		}
		{
			CheckpointText frozen;
			std::string expected;
			{
				AtomGroup group;
				for (int index = 0; index < 7; ++index) {
					auto atom = std::make_unique<Atom>();
					atom->SetOffset(Vector(index, -index));
					group.AddAtom(atom.release(), index % 3 + 1);
				}
				expected = group.SaveCheckpoint();
				{
					CheckpointWriter::BatchScope batch(true);
					frozen = CheckpointWriter::CaptureNative([&group] { return group.SaveCheckpoint(); });
				}
				group.RemoveAtoms(2);
			}
			const auto actual = std::async(std::launch::async, [frozen] { return frozen.Text(); }).get();
			check(actual == expected, "owned_atom_subgroups_resolve_indices_after_source_death");
		}
		{
			enum class SignedByte : int8_t { Low = -127 };
			const uint8_t unusualBool = 0xFE;
			bool flag;
			std::memcpy(&flag, &unusualBool, sizeof(flag));
			int8_t bytes[] = {-128, 0, 127};
			std::array<uint64_t, 2> wide{0, std::numeric_limits<uint64_t>::max()};
			const auto saveBlock = [&] {
				CheckpointWriter writer("PrimitiveBlock1");
				writer(flag, bytes, wide, SignedByte::Low, std::numeric_limits<int64_t>::min(),
				       -0.0F, std::bit_cast<float>(uint32_t{0x7FC00031}), std::bit_cast<double>(uint64_t{0x7FF8000000000031}));
				writer.PerPeer(bytes, wide);
				return writer.Text();
			};
			const auto ordinary = CheckpointWriter::CaptureNative(saveBlock);
			CheckpointText frozen;
			{
				CheckpointWriter::BatchScope batch(true);
				frozen = CheckpointWriter::CaptureNative(saveBlock);
			}
			flag = false; bytes[0] = 17; wide.fill(1);
			const auto archived = std::async(std::launch::async, [frozen] { return std::pair(frozen.Text(), frozen.SharedText()); }).get();
			check(frozen.SameValues(ordinary) && archived.first == ordinary.Text() && archived.second == ordinary.SharedText(),
			      "owned_primitive_blocks_preserve_width_bits_peer_runs_and_lifetime");
		}
		{
			CheckpointText frozen;
			std::string full, shared;
			{
				std::string binary("a\0b", 3);
				std::vector<Vector> positions{{-0.0F, std::bit_cast<float>(uint32_t{0x7FC00031})}, {17, 29}};
				Timer timer;
				timer.SetStartSimTimeTicks(37); timer.SetSimTimeLimitTicks(41);
				timer.SetStartRealTimeTicks(43); timer.SetRealTimeLimitTicks(47);
				const auto saveMixed = [&] {
					CheckpointWriter writer("MixedFields1");
					writer(-17, 19u, binary, -0.0F, 23, positions, 31, 37u, timer, 41, 43u, binary);
					writer.PerPeer(47, 53u, binary, 59, 61u);
					writer(67, 71u);
					return writer.Text();
				};
				const auto ordinary = CheckpointWriter::CaptureNative(saveMixed);
				full = ordinary.Text(); shared = ordinary.SharedText();
				CheckpointWriter::BatchScope batch(true);
				frozen = CheckpointWriter::CaptureNative(saveMixed);
				check(frozen.SameValues(ordinary), "mixed_primitive_runs_match_the_original_tape");
				binary.assign("changed"); positions.clear(); timer.Reset();
			}
			const auto archived = std::async(std::launch::async, [frozen] { return std::pair(frozen.Text(), frozen.SharedText()); }).get();
			check(archived.first == full && archived.second == shared,
			      "mixed_primitive_runs_outlive_strings_containers_and_clocks");
		}
		{
			CheckpointText frozen, ordinary;
			std::string full, shared;
			{
				std::array<std::string, 3> strings{std::string("a\0b\xff", 4), "", std::string(32769, 'x')};
				std::string array[] = {"", std::string("z\0y", 3)};
				const auto saveStrings = [&] {
					CheckpointWriter writer("OwnedStringBlock1");
					writer(strings, -0.0F, array, std::numeric_limits<int64_t>::min(), strings[2]);
					writer.PerPeer(array, std::bit_cast<double>(uint64_t{0x7FF8000000000031}), strings);
					return writer.Text();
				};
				ordinary = CheckpointWriter::CaptureNative(saveStrings);
				full = ordinary.Text(); shared = ordinary.SharedText();
				{
					CheckpointWriter::BatchScope batch(true);
					frozen = CheckpointWriter::CaptureNative(saveStrings);
				}
				strings.fill("changed"); array[0] = "changed";
			}
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(frozen.SameValues(ordinary) && actual.first == full && actual.second == shared,
			      "packed_string_blocks_preserve_binary_arrays_peer_lengths_and_source_lifetime");
		}
		{
			CheckpointText frozen, equal, changed;
			std::string full, shared;
			{
				std::string source(8193, 'q'); source[4096] = '\0';
				const auto write = [&](CheckpointBuffer& buffer) {
					for (int index = 0; index < 1024; ++index) {
						buffer.Integer(index, true);
						if (index == 63) buffer.String(source);
						if (index == 127) buffer.PeerBegin();
						if (index == 255) buffer.PeerEnd();
					}
				};
				CheckpointBuffer ordinary; write(ordinary);
				const auto expected = ordinary.Finish();
				full = expected.Text(); shared = expected.SharedText();
				CheckpointWriter::BatchScope batch(true);
				CheckpointBuffer::AllocationScope allocation(true);
				CheckpointBuffer first; write(first); frozen = first.Finish();
				first.Raw("fresh");
				check(first.Finish().Text() == "fresh", "published_checkpoint_chunks_survive_writer_reuse");
				CheckpointBuffer second; write(second); equal = second.Finish();
				source.back() = 'r';
				CheckpointBuffer third; write(third); changed = third.Finish();
				check(frozen.SameValues(equal) && !frozen.SameValues(changed) && frozen.m_Data->Output().tape.empty() &&
				    equal.m_Data->Output().tape.empty() && changed.m_Data->Output().tape.empty(), "checkpoint_chunk_comparison_preserves_bytes_without_flattening");
			}
			std::array<std::future<bool>, 4> readers;
			for (auto& reader: readers) reader = std::async(std::launch::async, [frozen, full, shared] {
				return frozen.Text() == full && frozen.SharedText() == shared;
			});
			bool exact = true;
			for (auto& reader: readers) exact = reader.get() && exact;
			check(exact, "owned_checkpoint_chunks_preserve_peer_bytes_after_scope_exit_for_concurrent_readers");
		}
		{
			struct InlineRecord {
				Vector position{-0.0F, std::bit_cast<float>(uint32_t{0x7FC00031})};
				Timer timer;
				std::string bytes{"x\0y", 3};
				std::string SaveCheckpoint() const {
					CheckpointWriter writer("InlineRecord1");
					writer(position, timer, bytes);
					return writer.Text();
				}
			};
			CheckpointText frozen;
			std::string fullReference, sharedReference;
			{
				std::vector<InlineRecord> records(512);
				for (auto& record: records) {
					record.timer.SetStartSimTimeTicks(17); record.timer.SetSimTimeLimitTicks(29);
					record.timer.SetStartRealTimeTicks(41); record.timer.SetRealTimeLimitTicks(53);
				}
				const auto saveInline = [&] {
					CheckpointWriter writer("InlineParent1"); writer(records);
					return writer.Text();
				};
				const auto ordinary = CheckpointWriter::CaptureNative(saveInline);
				fullReference = ordinary.Text(); sharedReference = ordinary.SharedText();
				CheckpointCache transient(true); transient.Begin();
				CheckpointWriter::CacheScope cache(&transient);
				CheckpointWriter::BatchScope batch(true);
				frozen = CheckpointWriter::CaptureNative(saveInline);
			}
			const auto archived = std::async(std::launch::async, [frozen] { return std::pair(frozen.Text(), frozen.SharedText()); }).get();
			check(frozen.HasPeerRuns() && archived.first == fullReference && archived.second == sharedReference,
			      "owned_inline_native_records_outlive_sources_and_keep_peer_lengths");
		}

	{
			CheckpointCache cache(true); cache.Begin();
			auto owner = std::make_unique<MOPixel>();
			const void* address = owner.get();
			cache.Remember(address, 7, CheckpointText("one"), 11, 13, owner.get());
			cache.Remember(address, 8, CheckpointText("two"), 17, 19, owner.get());
			int other = 0;
			cache.Remember(&other, 7, CheckpointText("three"), 23);
			bool kept = cache.Peek(address, 7)->Text() == "one" && cache.Peek(address, 8)->Text() == "two" &&
			            cache.Peek(&other, 7)->Text() == "three" && cache.Identity(address, 7) == 13;
			owner.reset();
			kept = kept && cache.Identity(address, 7) == 0 && cache.Stamp(address, 8) == 17;
			cache.Begin();
			kept = kept && !cache.PeekCurrent(address, 7) && cache.Touch(address, 7) && cache.PeekCurrent(address, 7);
			const auto retired = cache.RetireUnused();
			kept = kept && !cache.Peek(address, 8) && !cache.Peek(&other, 7) && cache.Peek(address, 7)->Text() == "one";
			const std::string census = cache.Census();
			check(kept && !retired.empty() && census.starts_with("entries=1 ") && census.find("channels=7:1") != std::string::npos,
			      "transient_checkpoint_cache_keeps_channels_generations_and_expired_owners");
		}
		{
			std::optional<CheckpointCache> cache;
			std::weak_ptr<std::pmr::memory_resource> storage;
			CheckpointText first("one"), second("two"), third("three");
			const void* address = nullptr;
			int other = 0;
			bool exact = true;
			{
				CheckpointWriter::BatchScope batch(true);
				CheckpointBuffer::AllocationScope allocation(true);
				cache.emplace(true); cache->Begin();
				storage = CheckpointBuffer::LeaseCaptureStorage();
				auto owner = std::make_unique<MOPixel>(); address = owner.get();
				cache->Remember(address, 7, first, 11, 13, owner.get());
				cache->Remember(address, 8, second, 17, 19, owner.get());
				cache->Remember(&other, 7, third, 23);
				exact = cache->m_CapturedEntries && cache->m_CapturedEntries->storage == storage.lock() && cache->m_Entries.empty() &&
				    cache->Peek(address, 7)->Text() == "one" && cache->Peek(address, 8)->Text() == "two" && cache->Identity(address, 7) == 13;
			}
			exact = !storage.expired() && cache->Identity(address, 7) == 0 && cache->Stamp(address, 8) == 17 && exact;
			CheckpointCache copy = *cache;
			copy.Begin();
			exact = copy.Touch(&other, 7) && copy.PeekCurrent(&other, 7) && !copy.PeekCurrent(address, 7) && cache->PeekCurrent(address, 7) && exact;
			cache->Begin();
			exact = !cache->PeekCurrent(address, 7) && cache->Touch(address, 7) && cache->PeekCurrent(address, 7) && exact;
			const auto retired = cache->RetireUnused();
			const std::string census = cache->Census();
			exact = !retired.empty() && !cache->Peek(address, 8) && !cache->Peek(&other, 7) && copy.Peek(&other, 7)->Text() == "three" && cache->Peek(address, 7)->Text() == "one" &&
			    census.starts_with("entries=1 ") && census.find("owners=1 ") != std::string::npos && census.find("channels=7:1") != std::string::npos && exact;
			cache.reset();
			check(exact && storage.expired(), "transient_checkpoint_entries_keep_capture_storage_channels_and_last_lease");
		}
		{
			const std::string type = "MOPixel", missing = "__checkpoint_preset_lookup_missing__";
			std::list<Entity*> candidates;
			g_PresetMan.GetAllOfType(candidates, type);
			const Entity* candidate = candidates.empty() ? nullptr : candidates.front();
			const int module = candidate ? candidate->GetModuleID() : (g_PresetMan.GetTotalModuleCount() ? 0 : -1);
			const Entity* expected = g_PresetMan.GetEntityPreset(type, missing, module);
			const Entity* candidateReference = candidate ? g_PresetMan.GetEntityPreset(candidate->GetClassName(), candidate->GetPresetName(), module) : nullptr;
			bool exact = true;
			bool fresh = expected == nullptr;
			{
				CheckpointWriter::BatchScope batch(true);
				BitmapPixelCaptureScope scope;
				std::vector<std::future<const Entity*>> readers;
				for (int reader = 0; reader < 16; ++reader) readers.push_back(std::async(std::launch::async, [&] {
					CheckpointCache cache(true); cache.Begin();
					return cache.FindPreset(type, missing, module);
				}));
				for (auto& reader: readers) exact = reader.get() == expected && exact;
				const size_t queries = module >= 0 ? readers.size() : 0;
				fresh = scope.m_State->presetQueries.load() == queries && fresh;
				if (candidate) {
					CheckpointCache first(true), second(true); first.Begin(); second.Begin();
					const auto lookup = [&](CheckpointCache& cache) { return cache.FindPreset(candidate->GetClassName(), candidate->GetPresetName(), candidate->GetModuleID()); };
					exact = lookup(first) == candidateReference && lookup(second) == candidateReference && exact;
					readers.clear();
					for (int reader = 0; reader < 16; ++reader) readers.push_back(std::async(std::launch::async, [&] {
						CheckpointCache cache(true); cache.Begin(); return lookup(cache);
					}));
					for (auto& reader: readers) exact = reader.get() == candidateReference && exact;
					exact = scope.m_State->presetQueries.load() == queries + 1 && exact;
				}
			}
			{
				CheckpointWriter::BatchScope batch(true);
				BitmapPixelCaptureScope scope;
				CheckpointCache cache(true); cache.Begin();
				if (candidate) exact = cache.FindPreset(candidate->GetClassName(), candidate->GetPresetName(), candidate->GetModuleID()) == candidateReference && scope.m_State->presetQueries.load() == 1 && exact;
				else exact = cache.FindPreset(type, missing, module) == expected && scope.m_State->presetQueries.load() == (module >= 0 ? 1 : 0) && exact;
			}
			check(exact, "checkpoint_preset_searches_are_shared_by_readers_and_reset_for_each_world_capture");
			check(fresh, "checkpoint_missing_presets_are_read_fresh_for_mods");
		}
		{
			CheckpointText frozen;
			std::string expected;
			bool exact = true;
			{
				constexpr int width = 7, height = 5;
				std::array<unsigned char, width * height> bytes;
				for (size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<unsigned char>(index * 17);
				expected.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
				std::array<unsigned char*, height> lines;
				for (int y = 0; y < height; ++y) lines[y] = bytes.data() + y * width;
				GFX_VTABLE vtable{}; vtable.color_depth = 8;
				BITMAP bitmap{}; bitmap.w = width; bitmap.h = height; bitmap.vtable = &vtable; bitmap.line = lines.data();
				CheckpointWriter::BatchScope batch(true);
				BitmapPixelCaptureScope scope;
				std::vector<std::future<CheckpointText>> readers;
				for (int reader = 0; reader < 16; ++reader) readers.push_back(std::async(std::launch::async, [&] {
					CheckpointCache cache(true); cache.Begin();
					return cache.CapturePixels(&bitmap);
				}));
				for (auto& reader: readers) {
					auto text = reader.get();
					if (!frozen.OwnedBytes()) frozen = text;
					else exact = frozen.SameValues(text) && exact;
				}
				size_t copies = 0;
				for (const auto& shard: scope.m_State->pixelShards) if (shard.values) copies += shard.values->size();
				exact = copies == 1 && exact;
				bytes.fill(0);
			}
			const auto actual = std::async(std::launch::async, [frozen] { return frozen.Text(); }).get();
			check(exact && actual == expected, "parallel_bitmap_readers_share_one_fresh_copy_after_source_death");
		}

		{
			CheckpointBuffer::WaitForPreparedStorageRelease();
			const size_t before = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed);
			CheckpointBuffer::PrepareCaptureStorage(8 << 20).get();
			std::shared_ptr<void> kept;
			std::weak_ptr<void> witness;
			{
				CheckpointWriter::BatchScope batch(true, true);
				kept = CheckpointBuffer::AllocateCaptureBytes(4096);
				witness = kept;
				std::memset(kept.get(), 0x5a, 4096);
			}
			CheckpointBuffer::WaitForPreparedStorageRelease();
			bool exact = !witness.expired() && s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) - before <= (1 << 20);
			exact = std::async(std::launch::async, [kept = std::move(kept)] {
				CheckpointWriter::BatchScope next(true);
				auto other = CheckpointBuffer::AllocateCaptureBytes(4096);
				std::memset(other.get(), 0xa5, 4096);
				const auto* bytes = static_cast<const uint8_t*>(kept.get());
				return std::all_of(bytes, bytes + 4096, [](uint8_t value) { return value == 0x5a; });
			}).get() && exact;
			check(exact && witness.expired() && s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) == before,
			    "prepared_pixel_bytes_retain_only_their_block_until_the_last_reader");
		}
		{
			CheckpointBuffer::WaitForPreparedStorageRelease();
			const size_t before = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed);
			auto prepared = std::make_shared<CheckpointArenaGroup::Prepared>();
			prepared->task = prepared->completion.get_future().share();
			const auto finished = prepared->task;
			std::promise<void> begin;
			const auto gate = begin.get_future().share();
			{
				std::lock_guard lock(s_ArenaPoolMutex);
				if (s_NextPrepared) throw std::logic_error("late preparation fixture found prior storage");
				s_NextPrepared = prepared;
			}
			g_ThreadMan.GetBackgroundThreadPool().push_task([prepared, gate]() mutable {
				auto completion = std::move(prepared->completion);
				try {
					gate.wait();
					for (int block = 0; block < 16; ++block) prepared->blocks.push_back(std::make_shared<CheckpointArenaGroup::Block>(1 << 20, alignof(std::max_align_t)));
					prepared->ready.store(true, std::memory_order_release);
					prepared.reset();
					completion.set_value();
				} catch (...) {
					prepared.reset();
					completion.set_exception(std::current_exception());
				}
			});
			prepared.reset();
			CheckpointText small;
			{
				CheckpointWriter::BatchScope batch(true, true);
				small = CheckpointWriter::CaptureNative([] { CheckpointWriter writer("LateArena1"); writer(17); return writer.Text(); });
			}
			begin.set_value();
			finished.get();
			CheckpointBuffer::WaitForPreparedStorageRelease();
			CheckpointWriter ordinary("LateArena1"); ordinary(17);
			check(s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) <= before + (2 << 20) && small.Text() == ordinary.Text(),
			      "late_prepared_checkpoint_storage_is_released_without_changing_owned_nodes");
			small = CheckpointText{};
			check(s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) <= before,
			      "late_prepared_checkpoint_storage_drops_its_last_lease");
		}


		{
			CheckpointBuffer::WaitForPreparedStorageRelease();
			const size_t before = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed);
			auto prepared = std::make_shared<CheckpointArenaGroup::Prepared>();
			prepared->task = prepared->completion.get_future().share();
			const auto finished = prepared->task;
			std::promise<void> begin;
			const auto gate = begin.get_future().share();
			{
				std::lock_guard lock(s_ArenaPoolMutex);
				if (s_NextPrepared) throw std::logic_error("late preparation fixture found prior storage");
				s_NextPrepared = prepared;
			}
			g_ThreadMan.GetBackgroundThreadPool().push_task([prepared, gate]() mutable {
				auto completion = std::move(prepared->completion);
				try {
					gate.wait();
					for (int block = 0; block < 16; ++block) prepared->blocks.push_back(std::make_shared<CheckpointArenaGroup::Block>(1 << 20, alignof(std::max_align_t)));
					prepared->ready.store(true, std::memory_order_release);
					prepared.reset();
					completion.set_value();
				} catch (...) {
					prepared.reset();
					completion.set_exception(std::current_exception());
				}
			});
			prepared.reset();
			CheckpointBuffer::CancelCaptureStorage();
			begin.set_value();
			finished.get();
			CheckpointBuffer::WaitForPreparedStorageRelease();
			check(s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) <= before,
			      "cancelled_late_checkpoint_preparation_releases_after_producer_finishes");
		}

		{
			CheckpointBuffer::WaitForPreparedStorageRelease();
			const size_t before = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed);
			const size_t released = s_PreparedReleaseWorkerBytes.load(std::memory_order_relaxed);
			CheckpointBuffer::PrepareCaptureStorage(16 << 20).get();
			CheckpointBuffer::CancelCaptureStorage();
			CheckpointBuffer::WaitForPreparedStorageRelease();
			check(s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) <= before &&
			      s_PreparedReleaseWorkerBytes.load(std::memory_order_relaxed) - released >= (16 << 20),
			      "cancelled_checkpoint_preparation_releases_all_unused_storage_on_worker");
		}

		{
			CheckpointBuffer::WaitForPreparedStorageRelease();
			const size_t before = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed);
			CheckpointText plain, peer;
			std::weak_ptr<const std::string> source;
			{
				CheckpointWriter::BatchScope batch(true);
				CheckpointBuffer::AllocationScope allocations(true);
				auto owned = std::make_shared<const std::string>("before\0death", 12);
				source = owned;
				plain = CheckpointText::Deferred([owned] { return *owned; }, owned->size());
				peer = CheckpointText::DeferredWithPeerRuns([] { return std::string("a<peer>gone<peer>b"); }, "<peer>", 17);
			}
			const bool retained = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) > before;
			const bool exact = std::async(std::launch::async, [plain, peer] {
				return plain.Text() == std::string("before\0death", 12) && peer.Text() == "agoneb" && peer.SharedText() == "ab";
			}).get();
			plain = CheckpointText{}; peer = CheckpointText{};
			check(retained && exact && source.expired() && s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) <= before,
			      "deferred_checkpoint_nodes_keep_and_release_capture_storage_on_workers");
		}
		{
			Timer timer;
			timer.SetStartSimTimeTicks(17); timer.SetSimTimeLimitTicks(29);
			timer.SetStartRealTimeTicks(41); timer.SetRealTimeLimitTicks(53);
			std::vector<Vector> vectors(512, Vector(-0.0F, std::bit_cast<float>(uint32_t{0x7fc00031})));
			const auto saveArena = [&] {
				CheckpointWriter outer("Arena1");
				for (const Vector& vector: vectors) outer(CheckpointWriter::Native([&] {
					CheckpointWriter inner("ArenaChild1");
					inner(vector, timer, std::string("x\0y", 3));
					return inner.Text();
				}));
				return outer.Text();
			};
			const CheckpointText ordinary = CheckpointWriter::CaptureNative(saveArena);
			CheckpointText frozen;
			{
				CheckpointWriter::BatchScope batches(true);
				frozen = CheckpointWriter::CaptureNative(saveArena);
			}
			vectors.clear(); timer.SetStartSimTimeTicks(91);
			const auto archived = std::async(std::launch::async, [frozen] { return std::pair(frozen.Text(), frozen.SharedText()); }).get();
			check(frozen.SameValues(ordinary) && archived.first == ordinary.Text() && archived.second == ordinary.SharedText(),
			      "owned_checkpoint_arena_outlives_capture_and_live_values");
		}

		{
			std::array<CheckpointText, 4> frozen;
			std::array<std::string, 4> reference;
			for (size_t index = 0; index < reference.size(); ++index) {
				CheckpointWriter writer("ArenaGroup1"); writer(index, std::string("before\0death", 12));
				reference[index] = writer.Text();
			}
			{
				CheckpointWriter::BatchScope batch(true);
				std::array<std::future<void>, 4> workers;
				for (size_t index = 0; index < workers.size(); ++index) workers[index] = std::async(std::launch::async, [&, index] {
					std::string source("before\0death", 12);
					for (int repetition = 0; repetition < 256; ++repetition) {
						frozen[index] = CheckpointWriter::CaptureNative([&] {
							CheckpointWriter writer("ArenaGroup1"); writer(index, source); return writer.Text();
						});
					}
					source.assign("after");
				});
				for (auto& worker: workers) worker.get();
			}
			bool survived = true;
			{
				CheckpointWriter::BatchScope next(true);
				std::array<std::future<bool>, 4> readers;
				for (size_t index = 0; index < readers.size(); ++index) readers[index] = std::async(std::launch::async, [&, index] {
					const auto fresh = CheckpointWriter::CaptureNative([] { CheckpointWriter writer("NextArena1"); writer(97); return writer.Text(); });
					return !fresh.Text().empty() && frozen[index].Text() == reference[index] && frozen[index].SharedText() == reference[index];
				});
				for (auto& reader: readers) survived = reader.get() && survived;
			}
			check(survived, "owned_checkpoint_pool_keeps_prior_thread_groups_during_a_new_capture");
		}

		{
			const size_t before = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed);
			CheckpointBuffer::PrepareCaptureStorage(16 << 20).get();
			CheckpointText small;
			{
				CheckpointWriter::BatchScope batch(true);
				small = CheckpointWriter::CaptureNative([] {
					CheckpointWriter writer("SmallArena1"); writer(17); return writer.Text();
				});
				{
					const std::string source(8 << 20, 'x');
					const auto discarded = CheckpointWriter::CaptureNative([&] {
						CheckpointWriter writer("LargeArena1"); writer(source); return writer.Text();
					});
					if (discarded.OwnedBytes() < source.size()) throw std::logic_error("large arena fixture lost its values");
				}
			}
			CheckpointWriter ordinary("SmallArena1"); ordinary(17);
			const size_t retained = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed);
			check(retained <= before + (2 << 20) && small.Text() == ordinary.Text(),
			      "small_cached_checkpoint_releases_unrelated_large_backing_blocks");
			check(retained <= before + (2 << 20), "prepared_checkpoint_storage_does_not_pin_unused_blocks");
			small = CheckpointText{};
			check(s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) <= before,
			      "checkpoint_pool_releases_its_last_backing_lease");
		}

		{
			CheckpointBuffer::WaitForPreparedStorageRelease();
			const size_t before = s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed);
			const size_t calls = s_PreparedReleaseWorkerCalls.load(std::memory_order_relaxed);
			const size_t bytes = s_PreparedReleaseWorkerBytes.load(std::memory_order_relaxed);
			CheckpointBuffer::PrepareCaptureStorage(16 << 20).get();
			CheckpointText small;
			{
				CheckpointWriter::BatchScope batch(true, true);
				small = CheckpointWriter::CaptureNative([] { CheckpointWriter writer("ReleasedArena1"); writer(17); return writer.Text(); });
			}
			CheckpointBuffer::WaitForPreparedStorageRelease();
			CheckpointWriter ordinary("ReleasedArena1"); ordinary(17);
			check(s_PreparedReleaseWorkerCalls.load(std::memory_order_relaxed) > calls &&
			      s_PreparedReleaseWorkerBytes.load(std::memory_order_relaxed) - bytes >= (15 << 20),
			      "prepared_checkpoint_spares_are_released_on_a_worker");
			check(s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) <= before + (2 << 20) && small.Text() == ordinary.Text(),
			      "prepared_checkpoint_worker_release_preserves_owned_nodes");
			small = CheckpointText{};
			check(s_CheckpointPoolLiveBytes.load(std::memory_order_relaxed) <= before,
			      "prepared_checkpoint_worker_release_drops_its_last_lease");
		}

		{
			Timer timer;
			timer.SetStartSimTimeTicks(17); timer.SetSimTimeLimitTicks(29);
			timer.SetStartRealTimeTicks(41); timer.SetRealTimeLimitTicks(53);
			std::vector<std::pair<int, Timer>> timers{{7, timer}, {9, timer}};
			const auto saveTimers = [&] { CheckpointWriter writer("OwnedTimers1"); writer(timer, timers); return writer.Text(); };
			const std::string before = saveTimers();
			const CheckpointText frozen = CheckpointWriter::CaptureNative(saveTimers);
			timer.SetStartSimTimeTicks(61); timers[0].second.SetStartRealTimeTicks(71); timers.clear();
			const std::string shared = frozen.SharedText();
			check(frozen.HasPeerRuns() && frozen.Text() == before && shared == "12 OwnedTimers1 17 29 2 7 17 29 9 17 29 ",
			      "owned_checkpoint_copies_inline_timers_and_container_peer_runs");
		}

		const CheckpointText bytes(std::string("x\0y", 3));
		CheckpointBuffer tokens;
		tokens.Raw("prefix|"); tokens.GraphString(bytes); tokens.Raw("|"); tokens.Base64(bytes, true);
		std::string tokenReference = "prefix|s3:"; tokenReference.append("x\0y", 3); tokenReference += "|eAB5";
		check(tokens.Finish().Text() == tokenReference, "owned_checkpoint_preserves_graph_strings_and_base64");

		const bool capturing = CheckpointWriter::IsCapturing();
		bool rejected = false;
		try {
			CheckpointWriter::CaptureNative([]() -> std::string { CheckpointWriter writer("Decorated1"); return "prefix" + writer.Text(); });
		} catch (const std::logic_error&) { rejected = true; }
		check(rejected && CheckpointWriter::IsCapturing() == capturing, "owned_checkpoint_refuses_uncaptured_text_and_restores_scope");

		const auto nested = [](int id) { CheckpointWriter inner("Nested1"); inner(id, std::string("payload")); return inner.Text(); };
		const auto composeWithNative = [&nested] {
			CheckpointWriter outer("Outer1");
			outer(CheckpointWriter::Native([&nested] { return nested(1); }));
			outer(CheckpointWriter::Native([&nested] { return nested(2); }));
			return outer.Text();
		};
		const std::string composedPlainly = composeWithNative();
		const CheckpointText composedUnderCapture = CheckpointWriter::CaptureNative(composeWithNative);
		check(!composedPlainly.empty() && composedUnderCapture.Text() == composedPlainly, "owned_checkpoint_keeps_two_nested_writers_under_native");

		std::string handRefusal;
		try {
			CheckpointWriter::CaptureNative([&nested] {
				CheckpointWriter outer("Outer1");
				outer(nested(1));
				outer(nested(2));
				return outer.Text();
			});
		} catch (const std::logic_error& error) { handRefusal = error.what(); }
		check(handRefusal == "a nested checkpoint value must be produced with CheckpointWriter::Native" &&
		          CheckpointWriter::IsCapturing() == capturing,
		      "owned_checkpoint_refuses_a_hand_composed_nested_value");

		CheckpointCache scopeCache;
		scopeCache.Begin();
		const int cacheOwner = 0;
		const CheckpointText firstReach = scopeCache.Remember(&cacheOwner, 7, CheckpointWriter::CaptureNative(composeWithNative));
		const CheckpointText secondReach = scopeCache.Remember(&cacheOwner, 7, CheckpointWriter::CaptureNative(composeWithNative));
		check(firstReach.SameValues(secondReach) && secondReach.Text() == composedPlainly && scopeCache.Reused() == 1,
		      "owned_checkpoint_cache_keeps_the_first_reach_identity_of_nested_writers");

		{
			struct RestoreClock {
				long long count = g_TimerMan.GetSimUpdateCount(), ticks = g_TimerMan.GetSimTickCount();
				~RestoreClock() { g_TimerMan.RestoreSimTickAfterPreview(count, ticks); }
			} restoreClock;
			constexpr int64_t firstTicks = 999960, secondTicks = 1999920;
			Timer past, future;
			past.SetStartSimTimeTicks(-1234567890123LL);
			future.SetStartSimTimeTicks(1500001);
			const auto saveTimers = [&](Writer& writer) {
				writer.NewPropertyWithValue("PastStart", past.GetStartSimTimeMS());
				writer.NewProperty("PastElapsed"); writer.ElapsedSimTime(past);
				writer.NewPropertyWithValue("FutureStart", future.GetStartSimTimeMS());
				writer.NewProperty("FutureElapsed"); writer.ElapsedSimTime(future);
			};
			const auto synchronous = [&] {
				auto stream = std::make_unique<std::ostringstream>();
				auto* output = stream.get();
				Writer writer(std::move(stream));
				writer << "timeless|";
				saveTimers(writer);
				return output->str();
			};
			auto timelessCalls = std::make_shared<std::atomic<int>>(0);
			const auto timeless = CheckpointText::Deferred([timelessCalls] { ++*timelessCalls; return std::string("timeless|"); });
			const auto capture = [&] {
				CheckpointBuffer buffer;
				buffer.Child(timeless);
				buffer.Child(Writer::Capture(saveTimers));
				return buffer.Finish();
			};
			CheckpointCache timerCache;
			timerCache.Begin();
			g_TimerMan.RestoreSimTickAfterPreview(60, firstTicks);
			const auto first = timerCache.Remember(&past, 0, capture());
			const auto firstImage = first.BindSimTime(firstTicks);
			const auto firstReference = synchronous();
			const bool signedElapsed = future.GetElapsedSimTimeMS() < 0;
			timerCache.Begin();
			g_TimerMan.RestoreSimTickAfterPreview(120, secondTicks);
			const auto fresh = capture();
			const auto second = timerCache.Remember(&past, 0, fresh);
			const auto secondImage = second.BindSimTime(secondTicks);
			const auto secondReference = synchronous();
			check(first.SameValues(fresh) && timerCache.Reused() == 1 && timelessCalls->load() == 0,
			      "owned_checkpoint_caches_timer_values_without_formatting_or_binding",
			      "same_values=" + std::to_string(first.SameValues(fresh)) + " reused=" + std::to_string(timerCache.Reused()) +
			          " timeless_calls=" + std::to_string(timelessCalls->load()));
			auto firstText = FloatingPointEnvironment::Async(std::launch::async, [firstImage] { return firstImage.Text(); });
			auto secondText = FloatingPointEnvironment::Async(std::launch::async, [secondImage] { return secondImage.Text(); });
			const std::string firstBound = firstText.get();
			const std::string secondBound = secondText.get();
			check(firstBound == firstReference && secondBound == secondReference && firstReference != secondReference &&
			          past.GetStartSimTimeMS() < 0 && signedElapsed && future.GetElapsedSimTimeMS() > 0,
			      "owned_checkpoint_binds_signed_timers_to_each_image_tick",
			      "first_bound=\"" + firstBound + "\" first_reference=\"" + firstReference + "\" second_bound=\"" + secondBound +
			          "\" second_reference=\"" + secondReference + "\" start_ms=" + std::to_string(past.GetStartSimTimeMS()) +
			          " signed_elapsed=" + std::to_string(signedElapsed) + " future_ms=" + std::to_string(future.GetElapsedSimTimeMS()));
			check(firstImage.Text() == firstReference && first.Text() == firstReference && fresh.Text() == secondReference && timelessCalls->load() == 1,
			      "owned_checkpoint_timer_images_preserve_prior_text_and_timeless_sharing",
			      "first_image=\"" + firstImage.Text() + "\" first=\"" + first.Text() + "\" first_reference=\"" + firstReference +
			          "\" fresh=\"" + fresh.Text() + "\" second_reference=\"" + secondReference + "\" timeless_calls=" +
			          std::to_string(timelessCalls->load()));
			CheckpointBuffer prior; prior.Raw("prior|"); prior.Child(first);
			CheckpointBuffer current; current.Raw("current|"); current.Child(fresh);
			const auto reusedTimers = current.Finish().ReuseChildren(prior.Finish());
			const std::string reusedText = reusedTimers.BindSimTime(secondTicks).Text();
			const std::string encodedText = first.Base64().BindSimTime(secondTicks).Text();
			check(reusedText == "current|" + secondReference && encodedText == base64_encode(secondReference, true),
			      "owned_checkpoint_binds_reused_and_encoded_timer_children",
			      "reused=\"" + reusedText + "\" expected=\"current|" + secondReference + "\" encoded=\"" + encodedText +
			          "\" expected_encoded=\"" + base64_encode(secondReference, true) + "\"");
		}

		{
			BS::thread_pool pool(2);
			const auto caller = std::this_thread::get_id();
			std::mutex mutex;
			std::condition_variable ready;
			bool release = false;
			std::atomic<bool> live{true};
			std::atomic<unsigned> completed{0}, expired{0};
			bool failed = false;
			try {
				CheckpointFailure::Scope failure(CheckpointFailure::Point::ParallelSubmission, 1);
				ParallelWork work(pool, 3, [&](size_t) {
					std::unique_lock lock(mutex);
					if (std::this_thread::get_id() == caller) { release = true; ready.notify_all(); }
					else ready.wait(lock, [&] { return release; });
					if (!live.load()) ++expired;
					++completed;
				}, 2);
			} catch (const std::bad_alloc&) { failed = true; }
			const unsigned atFailure = completed.load();
			live = false;
			{ std::lock_guard lock(mutex); release = true; ready.notify_all(); }
			pool.wait_for_tasks();
			check(failed && atFailure == 3 && expired.load() == 0,
			      "failed_parallel_submission_finishes_readers_before_the_caller_unwinds");
		}

		{
			CheckpointText frozen;
			std::weak_ptr<const std::string> owned;
			std::string reference;
			{
				CheckpointWriter::BatchScope batch(true);
				auto values = std::make_shared<const std::string>("owned|");
				owned = values;
				const auto capture = [&](std::string_view prefix, const CheckpointText& child) {
					CheckpointBuffer buffer;
					buffer.Raw(prefix);
					buffer.OwnedPrimitiveBlock(values, [](std::string& text, std::string_view data, bool tape) {
						if (data.size() != sizeof(const void*)) throw std::logic_error("invalid owned fixture block");
						const void* pointer;
						std::memcpy(&pointer, data.data(), sizeof(pointer));
						const auto& value = *static_cast<const std::string*>(pointer);
						if (tape) {
							text.push_back(static_cast<char>(CheckpointBuffer::ValueKind::Raw));
							const uint64_t size = value.size();
							text.append(reinterpret_cast<const char*>(&size), sizeof(size));
						}
						text += value;
					}, values->size());
					buffer.ElapsedSimTime(37, 1.0);
					buffer.Child(child);
					return buffer.Finish();
				};
				const auto previousChild = CheckpointText::Deferred([] { return std::string("child"); }, 0, "OwnedBlockChild1");
				const auto freshChild = CheckpointText::Deferred([] { return std::string("child"); }, 0, "OwnedBlockChild1");
				const auto previous = capture("previous|", previousChild);
				frozen = capture("current|", freshChild).ReuseChildren(previous).BindSimTime(91);
				CheckpointBuffer expected;
				expected.Raw("current|owned|"); expected.ElapsedSimTime(37, 1.0); expected.Raw("child");
				reference = expected.Finish().BindSimTime(91).Text();
			}
			const bool retained = !owned.expired();
			const std::string output = std::async(std::launch::async, [frozen] { return frozen.Text(); }).get();
			frozen = {};
			check(retained && output == reference && owned.expired(), "owned_primitive_blocks_survive_child_reuse_and_timer_rebinding");
		}

		{
			CheckpointCache sceneCache;
			CheckpointWriter::CacheScope cacheScope(&sceneCache);
			const auto capture = [](const SceneObject& object) {
				return Writer::Capture([&](Writer& writer) { Scene::SaveSceneObject(writer, &object, false, false); });
			};
			MOPixel pixel;
			pixel.Create();
			pixel.SetPos(Vector(10, 0));
			const long identity = pixel.GetUniqueID();
			const uint64_t stamp = pixel.CheckpointWriteGeneration();
			const auto before = capture(pixel);
			const auto unchanged = capture(pixel);
			pixel.Destroy();
			pixel.Create();
			Reader reader(std::make_unique<std::istringstream>(std::to_string(identity)), "checkpoint-identity", true);
			pixel.ReadProperty("UniqueID", reader);
			pixel.AdoptPersistedUniqueID();
			pixel.SetPos(Vector(20, 0));
			const auto restored = capture(pixel);
			std::cout << "[owned-checkpoint] restored_identity uid_kept=" << (pixel.GetUniqueID() == identity) << " generation_moved=" << (pixel.CheckpointWriteGeneration() != stamp)
			          << " unchanged_same=" << before.SameValues(unchanged) << " restored_same=" << before.SameValues(restored) << " text_differs=" << (before.Text() != restored.Text()) << std::endl;
			check(pixel.GetUniqueID() == identity && pixel.CheckpointWriteGeneration() != stamp &&
			          before.SameValues(unchanged) && !before.SameValues(restored) && before.Text() != restored.Text(),
			      "owned_checkpoint_does_not_reuse_a_restored_identity_at_the_same_address");

			Deployment deployment;
			deployment.SetPos(Vector(10, 0));
			const auto firstDeployment = capture(deployment);
			deployment.Reset();
			deployment.SetPos(Vector(20, 0));
			const auto nextDeployment = capture(deployment);
			check(firstDeployment.Text() != nextDeployment.Text(), "owned_checkpoint_recaptures_a_scene_object_without_a_lifetime_identity");
		}

		{
			auto actor = std::make_unique<Actor>();
			auto pixel = std::make_unique<MOPixel>();
			pixel->Create();
			pixel->SetPos(Vector(-0.0F, 13.5F));
			pixel->SetPresetName("owned inventory pixel");
			actor->AddInventoryItem(pixel.release());
			actor->SetPos(Vector(17.25F, -23.5F));
			const auto capture = [&] {
				return Writer::Capture([&](Writer& output) { Scene::SaveSceneObject(output, actor.get(), false, true); });
			};
			const std::string reference = capture().Text();
			CheckpointText frozen;
			{
				CheckpointWriter::BatchScope batch(true);
				CheckpointCache transient(true);
				transient.Begin();
				CheckpointWriter::CacheScope cacheScope(&transient);
				frozen = capture();
			}
			actor->SetPos(Vector(99, 101));
			actor.reset();
			const std::string encoded = std::async(std::launch::async, [frozen] { return frozen.Text(); }).get();
			check(encoded == reference, "flat_checkpoint_tree_owns_nested_inventory_after_source_death");
		}

		{
			auto actor = std::make_unique<Actor>();
			auto pixel = std::make_unique<MOPixel>();
			pixel->Create(); pixel->SetPos(Vector(-0.0F, 13.5F)); pixel->SetPresetName("native inventory pixel");
			actor->AddInventoryItem(pixel.release()); actor->SetPos(Vector(17.25F, -23.5F));
			actor->SetStringValue(std::string("a\0key", 5), std::string("v\0value", 7));
			actor->SetDescription(std::string("mod\0description", 15)); actor->AddToGroup("capture-only group");
			const auto capture = [](const Actor* value) {
				return Writer::Capture([&](Writer& writer) { Scene::SaveSceneObject(writer, value, false, true); });
			};
			const auto baseline = capture(actor.get());
			const std::string full = baseline.Text(), shared = baseline.SharedText();
			auto marker = std::make_unique<Actor>();
			Actor* const nextGameplaySlot = marker.get();
			marker.reset();
			const auto uid = MovableObject::GetUniqueIDCounter();
			const auto simDraws = g_SimRNG.GetDrawCount(), renderDraws = g_RenderRNG.GetDrawCount();
			bool refused = true;
			for (size_t after: {size_t{0}, size_t{1}}) {
				bool failed = false;
				try {
					CheckpointNativeSnapshot partial;
					CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects, after);
					partial.Object(actor.get());
				} catch (const std::bad_alloc&) { failed = true; }
				refused = refused && failed && capture(actor.get()).Text() == full;
			}
			auto snapshot = std::make_shared<CheckpointNativeSnapshot>();
			Actor* const frozen = snapshot->Object(actor.get());
			snapshot->SealBoundary();
			const bool aliases = snapshot->Object(actor.get()) == frozen && snapshot->ValueObject(actor->GetController()) == frozen->GetController();
			marker = std::make_unique<Actor>();
			const bool poolUnchanged = marker.get() == nextGameplaySlot;
			marker.reset();
			actor->SetPos(Vector(99, 101)); actor->SetStringValue(std::string("a\0key", 5), "changed");
			actor->SetDescription("changed description"); actor->RemoveFromGroup("capture-only group"); actor.reset();
			const auto output = std::async(std::launch::async, [snapshot, frozen, capture] {
				CheckpointFrozenClock other{123456789, 987654321, 111111111};
				CheckpointFrozenClock::Scope moved(&other);
				CheckpointNativeSnapshot::ReadScope read(snapshot.get());
				CheckpointWriter::BatchOverride batch(true);
				CheckpointWriter::CacheScope cache(nullptr);
				const auto values = capture(frozen);
				return std::pair{values.Text(), values.SharedText()};
			}).get();
			check(refused && aliases && poolUnchanged && output.first == full && output.second == shared && MovableObject::GetUniqueIDCounter() == uid &&
			      g_SimRNG.GetDrawCount() == simDraws && g_RenderRNG.GetDrawCount() == renderDraws,
			      "native_snapshot_retries_partial_failure_preserves_aliases_and_serializes_after_source_death");
		}

		{
			GATutorial source;
			const auto baseline = Writer::Capture([&](Writer& writer) { writer.NewPropertyWithValue("Activity", &source); });
			auto snapshot = std::make_shared<CheckpointNativeSnapshot>();
			const GATutorial* const frozen = snapshot->Object(&source);
			snapshot->SealBoundary();
			const auto output = std::async(std::launch::async, [snapshot, frozen] {
				CheckpointNativeSnapshot::ReadScope read(snapshot.get());
				CheckpointWriter::BatchOverride batch(true);
				const auto values = Writer::Capture([&](Writer& writer) { writer.NewPropertyWithValue("Activity", frozen); });
				return std::pair{values.Text(), values.SharedText()};
			}).get();
			check(output.first == baseline.Text() && output.second == baseline.SharedText(), "native_starting_tutorial_keeps_its_type_and_peer_values");
		}

		{
			auto actor = std::make_unique<Actor>();
			actor->SetStringValue("z", "last");
			actor->SetStringValue(std::string("a\0key", 5), std::string("first\0value", 11));
			actor->SetStringValue("a", "prefix");
			actor->SetNumberValue("z", -0.0);
			actor->SetNumberValue("a", std::bit_cast<double>(uint64_t{0x7FF8000000004321}));
			const auto capture = [&] {
				return Writer::Capture([&](Writer& output) { Scene::SaveSceneObject(output, actor.get(), false, false); });
			};
			const std::string reference = capture().Text();
			CheckpointText frozen;
			{
				CheckpointWriter::BatchScope batch(true);
				CheckpointCache transient(true);
				transient.Begin();
				CheckpointWriter::CacheScope cacheScope(&transient);
				frozen = capture();
			}
			actor->SetStringValue("a", "changed");
			actor->SetNumberValue("z", 99);
			actor.reset();
			std::array<std::future<std::string>, 4> readers;
			for (auto& reader: readers) reader = std::async(std::launch::async, [frozen] { return frozen.Text(); });
			bool exact = true;
			for (auto& reader: readers) exact = reader.get() == reference && exact;
			check(exact, "owned_custom_values_sort_after_source_death_with_binary_keys_and_float_bits");
		}

		{
			std::vector<CheckpointText> deferred;
			{
				CheckpointWriter::BatchScope batch(true);
				CheckpointBuffer::AllocationScope arena(true);
				for (int index = 0; index < 1024; ++index) {
					std::string live = std::to_string(index) + std::string("\0owned", 6);
					deferred.push_back(CheckpointText::Deferred([owned = live] { return owned; }, live.size()));
					live.assign("changed");
				}
				deferred.push_back(CheckpointText::DeferredWithPeerRuns([] { return std::string("first@private@last"); }, "@"));
			}
			const auto read = [deferred] {
				for (int index = 0; index < 1024; ++index) {
					if (deferred[index].Text() != std::to_string(index) + std::string("\0owned", 6)) return false;
				}
				return deferred.back().Text() == "firstprivatelast" && deferred.back().SharedText() == "firstlast";
			};
			std::array<std::future<bool>, 4> readers;
			for (auto& reader: readers) reader = std::async(std::launch::async, read);
			bool exact = true;
			for (auto& reader: readers) exact = reader.get() && exact;
			check(exact, "deferred_checkpoint_arena_survives_scope_exit_and_concurrent_peer_reads");
		}

		auto calls = std::make_shared<std::atomic<int>>(0);
		auto replacements = std::make_shared<std::atomic<int>>(0);
		const auto original = CheckpointText::Deferred([calls] { ++*calls; return std::string("stable"); }, 0, "OwnedCheckpointStable1");
		const auto replacement = CheckpointText::Deferred([replacements] { ++*replacements; return std::string("stable"); }, 0, "OwnedCheckpointStable1");
		CheckpointBuffer oldParent; oldParent.Raw("old|"); oldParent.Child(original);
		const auto previous = oldParent.Finish();
		const bool formatted = previous.Text() == "old|stable";
		CheckpointBuffer newParent; newParent.Raw("new|"); newParent.Child(replacement);
		const auto reused = newParent.Finish().ReuseChildren(previous);
		check(formatted && reused.Text() == "new|stable" && calls->load() == 1 && replacements->load() == 0, "owned_checkpoint_reuses_unchanged_child_producer");
		const auto unkeyed = CheckpointText::Deferred([] { return std::string("stable"); });
		const auto otherUnkeyed = CheckpointText::Deferred([] { return std::string("stable"); });
		check(unkeyed.SameValues(unkeyed) && !unkeyed.SameValues(otherUnkeyed) && !unkeyed.SameValues(original), "owned_checkpoint_preserves_deferred_identity_rules");
		// A formatted deferred text keeps its text, not its producer: what the producer captured (a frozen heap copy) is let go.
		auto held = std::make_shared<int>(7);
		const auto holding = CheckpointText::Deferred([held] { return std::to_string(*held); }, 0, "OwnedCheckpointHolding1");
		const long before = held.use_count();
		const bool producedOnce = holding.Text() == "7" && holding.Text() == "7" && holding.SameValues(holding);
		check(before == 2 && producedOnce && held.use_count() == 1, "owned_checkpoint_formatted_text_lets_its_producer_go",
		      "use_count before=" + std::to_string(before) + " after=" + std::to_string(held.use_count()));
		{
			auto calls = std::make_shared<std::atomic<unsigned>>(0);
			auto witness = std::make_shared<int>(17);
			const auto deferred = CheckpointText::DeferredValues([calls, witness] {
				if (calls->fetch_add(1) == 0) throw std::bad_alloc();
				CheckpointWriter::BatchOverride batch(true);
				return CheckpointWriter::CaptureNative([&] {
					CheckpointWriter writer("DeferredValuesProbe"); writer(*witness); writer.PerPeer(std::string("private\0value", 13)); return writer.Text();
				});
			});
			bool failed = false;
			try { deferred.Text(); } catch (const std::bad_alloc&) { failed = true; }
			const std::string full = deferred.Text(), shared = deferred.SharedText();
			check(failed && full != shared && full.find(std::string("private\0value", 13)) != std::string::npos && shared.find("private") == std::string::npos &&
			      calls->load() == 2 && witness.use_count() == 1 && deferred.Text() == full, "deferred_native_values_preserve_peer_runs_retry_and_release_the_snapshot");
		}

		{
			struct Producer {
				std::array<uint64_t, 257> fields{};
				std::shared_ptr<std::atomic<unsigned>> calls;
				std::shared_ptr<int> source;
				std::string operator()() {
					if (calls->fetch_add(1) == 0) throw std::runtime_error("captured producer retry");
					return std::to_string(fields.front()) + std::string("\0@private@", 10) + std::to_string(fields.back());
				}
			};
			CheckpointText frozen;
			std::weak_ptr<int> witness;
			auto calls = std::make_shared<std::atomic<unsigned>>(0);
			bool pooled = false;
			{
				Producer source; source.fields.front() = 31; source.fields.back() = 47; source.calls = calls; source.source = std::make_shared<int>(7);
				witness = source.source;
				CheckpointWriter::BatchScope batch(true);
				CheckpointBuffer::AllocationScope allocation(true);
				frozen = CheckpointText::DeferredWithPeerRuns(source, "@", sizeof(source));
				pooled = std::holds_alternative<CheckpointText::CapturedProducer>(frozen.m_Data->produce);
				source.fields.fill(0);
			}
			bool retried = false;
			try { frozen.Text(); } catch (const std::runtime_error&) { retried = true; }
			const bool retained = !witness.expired();
			std::array<std::future<bool>, 4> readers;
			for (auto& reader: readers) reader = std::async(std::launch::async, [frozen] {
				return frozen.Text() == std::string("31\0private47", 12) && frozen.SharedText() == std::string("31\0" "47", 5);
			});
			bool exact = true;
			for (auto& reader: readers) exact = reader.get() && exact;
			check(pooled && retried && retained && exact && calls->load() == 2 && witness.expired(),
			    "captured_producer_storage_keeps_owned_fields_retries_and_concurrent_publication");
			CheckpointText discarded;
			{
				Producer source; source.calls = calls; source.source = std::make_shared<int>(9);
				witness = source.source;
				CheckpointWriter::BatchScope batch(true);
				CheckpointBuffer::AllocationScope allocation(true);
				discarded = CheckpointText::Deferred(source);
			}
			const bool kept = !witness.expired();
			discarded = CheckpointText();
			check(kept && witness.expired() && calls->load() == 2, "captured_producer_releases_unformatted_fields_after_arena_scope");
		}
		{
			struct EndBatchOnMove {
				std::string value;
				std::unique_ptr<CheckpointWriter::BatchScope>* batch;
				EndBatchOnMove(std::string text, std::unique_ptr<CheckpointWriter::BatchScope>* owner) : value(std::move(text)), batch(owner) {}
				EndBatchOnMove(const EndBatchOnMove&) = default;
				EndBatchOnMove(EndBatchOnMove&& other) noexcept : value(std::move(other.value)), batch(other.batch) { batch->reset(); }
				std::string operator()() { return value; }
			};
			CheckpointText frozen;
			{
				auto batch = std::make_unique<CheckpointWriter::BatchScope>(true);
				CheckpointBuffer::AllocationScope allocation(true);
				EndBatchOnMove source("retained after batch construction", &batch);
				frozen = CheckpointText::Deferred(std::move(source));
			}
			const auto actual = std::async(std::launch::async, [frozen] { return std::pair{frozen.Text(), frozen.SharedText()}; }).get();
			check(actual.first == "retained after batch construction" && actual.second == actual.first,
			    "captured_producer_retains_its_arena_when_construction_ends_the_batch");
		}
		auto attempts = std::make_shared<std::atomic<int>>(0);
		const auto flaky = CheckpointText::Deferred([attempts] {
			if (attempts->fetch_add(1) == 0) throw std::runtime_error("owned checkpoint retry probe");
			return std::string("tail");
		});
		CheckpointBuffer retryBuffer; retryBuffer.Raw("prefix|"); retryBuffer.Child(flaky);
		const auto retry = retryBuffer.Finish();
		bool failed = false;
		try { retry.Text(); } catch (const std::runtime_error&) { failed = true; }
		check(failed && retry.Text() == "prefix|tail" && retry.Text() == "prefix|tail" && attempts->load() == 2, "owned_checkpoint_retry_publishes_no_partial_prefix");

		auto concurrentCalls = std::make_shared<std::atomic<int>>(0);
		const auto concurrent = CheckpointText::Deferred([concurrentCalls] { ++*concurrentCalls; return std::string("shared"); });
		CheckpointBuffer concurrentBuffer; concurrentBuffer.Raw("read|"); concurrentBuffer.Child(concurrent);
		const auto readersText = concurrentBuffer.Finish();
		std::vector<std::future<std::string>> readers;
		std::promise<void> start;
		const auto ready = start.get_future().share();
		for (int index = 0; index < 4; ++index) readers.push_back(FloatingPointEnvironment::Async(std::launch::async, [readersText, ready] { ready.wait(); return readersText.Text(); }));
		start.set_value();
		bool same = true;
		for (auto& reader: readers) same = reader.get() == "read|shared" && same;
		check(same && concurrentCalls->load() == 1, "owned_checkpoint_concurrent_readers_share_publication");

		const auto chain = [](CheckpointText value) {
			for (int depth = 0; depth < 32768; ++depth) { CheckpointBuffer parent; parent.Child(value); value = parent.Finish(); }
			return value;
		};
		auto payload = std::make_shared<int>(7);
		const std::weak_ptr<int> witness(payload);
		auto first = chain(CheckpointText::Deferred([payload] { return std::string("a"); }, sizeof(int), "OwnedCheckpointDeepA1"));
		payload.reset();
		auto equal = chain(CheckpointText::Deferred([] { return std::string("a"); }, 0, "OwnedCheckpointDeepA1"));
		auto changed = chain(CheckpointText(std::string("b")));
		auto deep = FloatingPointEnvironment::Async(std::launch::async, [first = std::move(first), equal = std::move(equal), changed = std::move(changed), witness]() mutable {
			bool result = !witness.expired() && first.SameValues(equal) && !first.SameValues(changed);
			auto reused = changed.ReuseChildren(first);
			result = reused.SameValues(changed) && reused.Text() == "b" && first.Text() == "a" && result;
			reused = {}; changed = {}; equal = {}; first = {};
			return result && witness.expired();
		});
		check(deep.get(), "owned_checkpoint_deep_graph_compares_formats_and_releases_on_worker");
	} catch (const std::exception& error) {
		std::cout << "[script-graph-selftest] owned checkpoint exception: " << error.what() << std::endl;
		check(false, "owned_checkpoint_no_unexpected_exception");
	}
	return passed;
}
