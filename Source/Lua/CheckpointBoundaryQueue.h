#pragma once

#include "CheckpointBoundary.h"
#include "FloatingPointEnvironment.h"

#include <atomic>
#include <functional>
#include <future>
#include <thread>
#include <type_traits>

namespace RTE::CheckpointLua {

	template<class Header> class BoundaryQueue {
		static_assert(std::is_trivially_copyable_v<Header>);
	public:
		using Prepare = std::function<std::unique_ptr<Boundary>()>;
		using Complete = std::function<bool(const Header&, const Boundary&)>;
		BoundaryQueue(Prepare prepare, Complete complete) : m_Prepare(std::move(prepare)), m_Complete(std::move(complete)),
		    m_Worker([this](std::stop_token stop) { Work(stop); }) {}
		~BoundaryQueue() {
			m_Worker.request_stop(); Wake(); m_Worker.join();
		}
		BoundaryQueue(const BoundaryQueue&) = delete;
		BoundaryQueue& operator=(const BoundaryQueue&) = delete;

		// Only world setup waits for the initial inventory.
		bool WaitUntilPrepared() const {
			for (;;) {
				const State state = m_State.load(std::memory_order_acquire);
				if (state == State::Ready) return true;
				if (state == State::Failed || state == State::Stopped) return false;
				m_State.wait(state, std::memory_order_acquire);
			}
		}
		void NotifyNextTick() noexcept {
			State expected = State::Failed;
			if (m_State.compare_exchange_strong(expected, State::Preparing, std::memory_order_acq_rel)) m_State.notify_all();
			m_Refresh.store(true, std::memory_order_release); Wake();
		}
		bool Ready() const noexcept { return m_State.load(std::memory_order_acquire) == State::Ready; }
		bool TryCapture(const Header& header, std::shared_future<bool>& result) {
			State expected = State::Ready;
			if (!m_State.compare_exchange_strong(expected, State::Claimed, std::memory_order_acq_rel)) return false;
			try {
				if (m_Plan->boundary->Arm()) {
					std::memcpy(&m_Plan->header, &header, sizeof(Header));
					result = m_Plan->result;
					Publish(State::Armed); Wake();
					return true;
				}
			} catch (...) {
				Publish(State::Rebuild); Wake();
				throw;
			}
			Publish(State::Rebuild); Wake();
			return false;
		}

	private:
		enum class State { Preparing, Ready, Claimed, Armed, Working, Rebuild, Failed, Stopped };
		struct Plan {
			explicit Plan(std::unique_ptr<Boundary> prepared) : boundary(std::move(prepared)), result(done.get_future().share()) {}
			std::unique_ptr<Boundary> boundary;
			std::promise<bool> done;
			std::shared_future<bool> result;
			Header header;
		};
		Prepare m_Prepare;
		Complete m_Complete;
		std::unique_ptr<Plan> m_Plan;
		std::atomic<State> m_State{State::Preparing};
		std::atomic<bool> m_Refresh{false};
		std::atomic<uint64_t> m_Wake{0};
		std::jthread m_Worker;
		void Wake() noexcept { m_Wake.fetch_add(1, std::memory_order_release); m_Wake.notify_one(); }
		void Publish(State state) noexcept { m_State.store(state, std::memory_order_release); m_State.notify_all(); }
		void PreparePlan() noexcept {
			m_Plan.reset();
			try {
				CheckpointNativeStorage::CaptureScope allocations;
				auto boundary = m_Prepare();
				if (!boundary) throw std::runtime_error("checkpoint inventory preparation returned no boundary");
				m_Plan = std::make_unique<Plan>(std::move(boundary));
				Publish(State::Ready);
			} catch (...) { Publish(State::Failed); }
		}
		void Work(std::stop_token stop) noexcept {
			FloatingPointEnvironment::Initialize();
			const FloatingPointEnvironment::Scope environment("checkpoint saver");
			while (!stop.stop_requested()) {
				const uint64_t wake = m_Wake.load(std::memory_order_acquire);
				const State state = m_State.load(std::memory_order_acquire);
				if (state == State::Preparing || state == State::Rebuild) {
					Publish(State::Preparing); PreparePlan(); continue;
				}
				if (state == State::Armed) {
					Publish(State::Working);
					bool completed = false;
					try {
						CheckpointNativeStorage::CaptureScope allocations;
						m_Plan->boundary->Drain();
						completed = m_Complete(m_Plan->header, *m_Plan->boundary);
					} catch (...) {}
					m_Plan->done.set_value(completed);
					// The image and all its fences retire before the next preparation.
					Publish(State::Preparing); continue;
				}
				if (m_Refresh.exchange(false, std::memory_order_acq_rel)) {
					if (state == State::Failed) { Publish(State::Preparing); continue; }
					if (state == State::Ready && !m_Plan->boundary->Current()) {
						State expected = State::Ready;
						if (m_State.compare_exchange_strong(expected, State::Preparing, std::memory_order_acq_rel)) {
							m_State.notify_all(); continue;
						}
					}
				}
				m_Wake.wait(wake, std::memory_order_acquire);
			}
			if (m_Plan && m_State.load(std::memory_order_acquire) == State::Armed) m_Plan->done.set_value(false);
			m_Plan.reset(); Publish(State::Stopped);
		}
	};

	inline std::string BoundaryQueueSelfTestMismatch() {
		CheckpointNativeStorage::AllocationScope allocations(true);
		auto owner = HeapOwner::Create();
		lua_State* const lua = owner->State();
		struct Value : CheckpointNativeAllocated { uint64_t number = 7; };
		auto value = std::make_unique<Value>();
		const Value* const identity = value.get();
		struct RootHeader { const Value* value; uint64_t tick; } root{identity, 1};
		struct Header { uint64_t tick; };
		const std::array<const lua_State*, 1> states{lua};
		const std::array roots{Boundary::Root::Of(root)};
		const auto simulation = std::this_thread::get_id();
		std::atomic<bool> refused{false}, wrongThread{false}, inject{false};
		std::atomic<size_t> completions{0};
		std::promise<void> entered, release;
		const auto released = release.get_future().share();
		struct Release {
			std::promise<void>& promise;
			bool sent = false;
			void Send() { if (!sent) { promise.set_value(); sent = true; } }
			~Release() { Send(); }
		};
		ProtectedCall(lua, [&] { lua_pushinteger(lua, 11); lua_setglobal(lua, "queuedBoundaryValue"); });
		BoundaryQueue<Header> queue([&] {
			if (std::this_thread::get_id() == simulation || !FloatingPointEnvironment::IsValid()) wrongThread.store(true, std::memory_order_release);
			if (inject.exchange(false)) {
				CheckpointFailure::Scope failure(CheckpointFailure::Point::NativeObjects);
				try { Boundary::Prepare(states, roots); }
				catch (const std::bad_alloc&) { refused.store(failure.Triggered(), std::memory_order_release); throw; }
			}
			return Boundary::Prepare(states, roots);
		}, [&](const Header& header, const Boundary& boundary) {
			if (std::this_thread::get_id() == simulation || !FloatingPointEnvironment::IsValid()) return false;
			const size_t capture = completions.fetch_add(1);
			if (capture == 0) { entered.set_value(); released.wait(); }
			CheckpointNativeStorage::ReadScope read(boundary.NativeViews());
			const RootHeader* frozen = CheckpointNativeStorage::Source(&root);
			const TValue number = boundary.LuaHeap(0).GlobalValue("queuedBoundaryValue");
			return frozen->tick == header.tick && frozen->value == identity &&
			       CheckpointNativeStorage::Source(identity)->number == (capture ? 19 : 13) &&
			       tvisnumber(&number) && numberVnum(&number) == (capture ? 23 : 17);
		});
		Release unblock{release};
		if (!queue.WaitUntilPrepared() || wrongThread.load(std::memory_order_acquire)) return "boundary inventory ran on the simulation thread";
		root.tick = 2; value->number = 13;
		ProtectedCall(lua, [&] { lua_pushinteger(lua, 17); lua_setglobal(lua, "queuedBoundaryValue"); });
		std::shared_future<bool> first, overlap;
		if (!queue.TryCapture({2}, first)) return "prepared queued boundary refused";
		entered.get_future().wait();
		if (first.wait_for(std::chrono::seconds(0)) == std::future_status::ready || queue.TryCapture({2}, overlap))
			return "queued boundary joined the saver or accepted an overlapping image";
		root.tick = 3; value->number = 19;
		ProtectedCall(lua, [&] { lua_pushinteger(lua, 23); lua_setglobal(lua, "queuedBoundaryValue"); });
		unblock.Send();
		if (!first.get() || !queue.WaitUntilPrepared()) return "deferred boundary lost its frozen tick";
		const size_t growthBytes = std::async(std::launch::async, [&] { return owner->PrepareFreeze()->Image().ByteCount() + Snapshot::c_PageBytes; }).get();
		const std::string growth(growthBytes, 'x');
		ProtectedCall(lua, [&] { lua_pushlstring(lua, growth.data(), growth.size()); lua_setglobal(lua, "queuedGrowth"); });
		inject.store(true);
		if (queue.TryCapture({3}, overlap)) return "queued boundary accepted a grown heap inventory";
		if (queue.WaitUntilPrepared() || !refused.load(std::memory_order_acquire)) return "queued inventory allocation refusal was not retained";
		queue.NotifyNextTick();
		if (!queue.WaitUntilPrepared()) return "queued inventory could not retry allocation refusal";
		if (!queue.TryCapture({3}, overlap)) return "retried queued boundary refused";
		root.tick = 4; value.reset();
		return overlap.get() ? std::string() : "retried queued boundary did not survive source destruction";
	}
}
