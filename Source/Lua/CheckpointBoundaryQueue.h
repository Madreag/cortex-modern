#pragma once

#include "CheckpointBoundary.h"
#include "SteadyClock.h"
#include "FloatingPointEnvironment.h"
#include "ThreadMan.h"
#include "System.h"

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <type_traits>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

namespace RTE::CheckpointLua {

	template<class Header> class BoundaryQueue {
		static_assert(std::is_nothrow_copy_assignable_v<Header>);
	public:
		using Prepare = std::function<std::unique_ptr<Boundary>()>;
		using Complete = std::function<bool(const Header&, const Boundary&)>;
		using Refused = std::function<void(const Header&, std::exception_ptr)>;
		BoundaryQueue(Prepare prepare, Complete complete, Refused refused = {}) : m_Prepare(std::move(prepare)), m_Complete(std::move(complete)), m_Refused(std::move(refused)),
		    m_Worker([this](std::stop_token stop) { Work(stop); }) {}
		~BoundaryQueue() {
			m_Worker.request_stop(); Wake(); m_Worker.join();
		}
		BoundaryQueue(const BoundaryQueue&) = delete;
		BoundaryQueue& operator=(const BoundaryQueue&) = delete;

		// Only world setup waits for the initial inventory.
		bool WaitUntilPrepared() const {
			const SaverThreads::Lend lend;
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
		/// Whether the saver is still on its way to a prepared plan: writing a capture, retiring it, or preparing the next one.
		bool Busy() const noexcept {
			const State state = m_State.load(std::memory_order_acquire);
			return state != State::Ready && state != State::Failed && state != State::Stopped;
		}
		/// Whether a capture is near: the saver then commits and warms the prepared plan's copies, and otherwise gives them back.
		void SetWarm(bool warm) noexcept {
			HeapOwner::KeepCopiesWarm(warm);
			if (m_WantWarm.exchange(warm, std::memory_order_acq_rel) != warm) {
				if (warm) m_WarmAskedAt.store(RTE::SteadyClock::now().time_since_epoch().count(), std::memory_order_release);
				Wake();
			}
		}
		/// Waits up to the given time for a prepared plan the saver has warmed; false if none is warm by then.
		bool WaitUntilWarm(std::chrono::milliseconds limit) const {
			const SaverThreads::Lend lend;
			const auto until = RTE::SteadyClock::now() + limit;
			while (!(m_State.load(std::memory_order_acquire) == State::Ready && m_Warmed.load(std::memory_order_acquire))) {
				const State state = m_State.load(std::memory_order_acquire);
				if (state == State::Failed || state == State::Stopped || RTE::SteadyClock::now() >= until) return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}
		/// Why a capture asked for now would not be taken (empty when it would), and whether the queue failed rather than being busy.
		std::pair<std::string, bool> NotReadyState() const {
			switch (m_State.load(std::memory_order_acquire)) {
				case State::Ready: return {};
				case State::Preparing: {
					if (m_Releasing.load(std::memory_order_acquire)) return {"the last capture's inventory is still being released" + PreparationCosts(), false};
					// A preparation refused before keeps its reason while the next one runs.
					std::lock_guard lock(m_FailureMutex);
					if (m_PrepareFailure.empty()) return {"the checkpoint inventory is being prepared" + PreparationCosts(), false};
					const uint64_t refusals = m_Refusals.load(std::memory_order_acquire);
					return {"the checkpoint inventory is being prepared again after " + std::to_string(refusals) + (refusals == 1 ? " refused preparation" : " refused preparations") +
					    " (the last refused: " + m_PrepareFailure + ")" + PreparationCosts(), false};
				}
				case State::Rebuild: return {"the checkpoint inventory is being rebuilt", false};
				case State::Claimed: case State::Armed: case State::Working: return {"the previous capture is still being written", false};
				case State::Stopped: return {"the checkpoint saver has stopped", true};
				case State::Failed: {
					std::lock_guard lock(m_FailureMutex);
					return {"the checkpoint inventory could not be prepared" + (m_PrepareFailure.empty() ? std::string() : ": " + m_PrepareFailure), true};
				}
			}
			return {"the checkpoint saver is in an unknown state", true};
		}
		bool TryCapture(const Header& header, std::shared_future<bool>& result) {
			return TryCapture(result, [&header](Header& target) noexcept {
				if constexpr (std::is_trivially_copyable_v<Header>) std::memcpy(&target, &header, sizeof(Header));
				else target = header;
			});
		}
		template<class Fill> bool TryCapture(std::shared_future<bool>& result, const Fill& fill) {
			static_assert(std::is_nothrow_invocable_v<Fill, Header&>);
			m_ArmRefusal = nullptr; m_ArmStale = false;
			State expected = State::Ready;
			if (!m_State.compare_exchange_strong(expected, State::Claimed, std::memory_order_acq_rel)) return false;
			try {
				m_Plan->failure = CheckpointFailure::Capture();
				fill(m_Plan->header);
				std::array<int64_t, 7>* parts = nullptr;
				if constexpr (requires { m_Plan->header.BoundaryParts(); }) parts = m_Plan->header.BoundaryParts();
				const bool wanted = m_WantWarm.load(std::memory_order_acquire);
				const auto asked = RTE::SteadyClock::time_point(RTE::SteadyClock::duration(m_WarmAskedAt.load(std::memory_order_acquire)));
				m_Plan->boundary->NoteWarm(m_Warmed.load(std::memory_order_acquire),
				    wanted ? std::chrono::duration_cast<std::chrono::microseconds>(RTE::SteadyClock::now() - asked).count() : -1, m_LastWarmUs.load(std::memory_order_acquire));
				if (m_Plan->boundary->Arm(parts)) {
					// Prepared slots retain immutable owners without constructing their payloads here.
					result = m_Plan->result;
					Publish(State::Armed); Wake();
					return true;
				}
				m_ArmRefusal = m_Plan->boundary->Refusal(); m_ArmStale = m_Plan->boundary->RefusedStale();
			} catch (...) {
				Publish(State::Rebuild); Wake();
				throw;
			}
			Publish(State::Rebuild); Wake();
			return false;
		}
		/// Why the last capture's arm refused its plan, null when no plan was armed; and whether the world outgrew that plan.
		const char* ArmRefusal() const noexcept { return m_ArmRefusal; }
		bool ArmRefusedStale() const noexcept { return m_ArmStale; }
		/// Waits, the game lending the saver its time at the game's priority, for the plan a capture refused at this tick needs:
		/// one prepared again for a world that outgrew the last, or one still being prepared. False when none comes: the saver
		/// is still writing, or has stopped or failed.
		bool WaitForNewPlan() const {
			const State state = m_State.load(std::memory_order_acquire);
			if (state != State::Preparing && state != State::Rebuild && state != State::Ready) return false;
			const SaverThreads::Await await;
			return WaitUntilPrepared();
		}

	private:
		enum class State { Preparing, Ready, Claimed, Armed, Working, Rebuild, Failed, Stopped };
		struct Plan {
			explicit Plan(std::unique_ptr<Boundary> prepared) : boundary(std::move(prepared)), result(done.get_future().share()) {}
			std::unique_ptr<Boundary> boundary;
			std::promise<bool> done;
			std::shared_future<bool> result;
			Header header{};
			CheckpointFailure::Token failure;
		};
		Prepare m_Prepare;
		Complete m_Complete;
		Refused m_Refused;
		std::unique_ptr<Plan> m_Plan;
		std::atomic<State> m_State{State::Preparing};
		std::atomic<bool> m_Refresh{false};
		std::atomic<bool> m_WantWarm{false};
		int m_PlanWarm = -1; //!< The worker's: the prepared plan last warmed (1), cooled (0), or neither yet (-1).
		std::atomic<bool> m_Warmed{false}; //!< The prepared plan is warm, for a thread that waits on it.
		std::atomic<RTE::SteadyClock::rep> m_WarmAskedAt{0}; //!< When a capture last became near, for the arm's line.
		std::atomic<int64_t> m_LastWarmUs{0}; //!< How long the worker's last finished warm took.
		std::atomic<bool> m_Releasing{false}; //!< The last capture's plan is being torn down before the next preparation.
		const char* m_ArmRefusal = nullptr; //!< The capturing thread's: why its last arm refused.
		bool m_ArmStale = false;
		std::atomic<uint64_t> m_Preparations{0}, m_LastReleaseUs{0}, m_LastPrepareUs{0}; //!< Finished preparations and what the last one cost.
		std::atomic<uint64_t> m_Refusals{0}; //!< Preparations refused since the last one that finished; each is tried again at the next tick.
		/// How often and how slowly the inventory has been prepared, for a capture that found it not ready.
		std::string PreparationCosts() const {
			const uint64_t count = m_Preparations.load(std::memory_order_acquire);
			if (!count) return {};
			return " (" + std::to_string(count) + " prepared so far; the last released its predecessor in " + std::to_string(m_LastReleaseUs.load(std::memory_order_relaxed) / 1000) +
			    " ms and prepared in " + std::to_string(m_LastPrepareUs.load(std::memory_order_relaxed) / 1000) + " ms)";
		}
		std::atomic<uint64_t> m_Wake{0};
		mutable std::mutex m_FailureMutex;
		std::string m_PrepareFailure; //!< Why the last preparation was refused.
		void NotePrepareFailure(std::exception_ptr error) noexcept {
			try {
				std::string what = "an unknown error";
				try { std::rethrow_exception(error); } catch (const std::exception& failure) { what = failure.what(); } catch (...) {}
				std::lock_guard lock(m_FailureMutex);
				m_PrepareFailure = std::move(what);
			} catch (...) {}
		}
		std::jthread m_Worker;
		void Wake() noexcept { m_Wake.fetch_add(1, std::memory_order_release); m_Wake.notify_one(); }
		void Publish(State state) noexcept { m_State.store(state, std::memory_order_release); m_State.notify_all(); }
		void PreparePlan() noexcept {
			const auto began = RTE::SteadyClock::now();
			m_Releasing.store(true, std::memory_order_release);
			m_Plan.reset();
			m_Releasing.store(false, std::memory_order_release);
			const auto released = RTE::SteadyClock::now();
			try {
				CheckpointNativeStorage::CaptureScope allocations;
				auto boundary = m_Prepare();
				if (!boundary) throw std::runtime_error("checkpoint inventory preparation returned no boundary");
				m_Plan = std::make_unique<Plan>(std::move(boundary));
				m_PlanWarm = -1;
				m_Warmed.store(false, std::memory_order_release);
				const auto micros = [](auto from, auto to) { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count()); };
				m_LastReleaseUs.store(micros(began, released), std::memory_order_relaxed);
				m_LastPrepareUs.store(micros(released, RTE::SteadyClock::now()), std::memory_order_relaxed);
				m_Preparations.fetch_add(1, std::memory_order_acq_rel);
				m_Refusals.store(0, std::memory_order_release);
				try { std::lock_guard lock(m_FailureMutex); m_PrepareFailure.clear(); } catch (...) {}
				Publish(State::Ready);
			} catch (...) {
				NotePrepareFailure(std::current_exception());
				m_Refusals.fetch_add(1, std::memory_order_acq_rel);
				Publish(State::Failed);
			}
		}
		void Work(std::stop_token stop) noexcept {
			FloatingPointEnvironment::Initialize();
			// Initial world setup waits for preparation; later capture work can yield to gameplay.
			SaverThreads::Mark();
			const FloatingPointEnvironment::Scope environment("checkpoint saver");
			while (!stop.stop_requested()) {
				const uint64_t wake = m_Wake.load(std::memory_order_acquire);
				const State state = m_State.load(std::memory_order_acquire);
				if (state == State::Preparing || state == State::Rebuild) {
					Publish(State::Preparing); PreparePlan(); continue;
				}
				if (state == State::Armed) {
					SaverThreads::Enter();
					Publish(State::Working);
					bool completed = false;
					try {
						CheckpointFailure::ForwardScope failure(m_Plan->failure);
						CheckpointNativeStorage::CaptureScope allocations;
						const CheckpointNativeStorage::CoolingPause cooling;
						m_Plan->boundary->Drain();
						completed = m_Complete(m_Plan->header, *m_Plan->boundary);
					} catch (...) {
						if (m_Refused) { try { m_Refused(m_Plan->header, std::current_exception()); } catch (...) {} }
					}
					m_Plan->done.set_value(completed);
					// Test lever CCCP_TEST_SLOW_CAPTURE_RETIRE_MS=<ms>: a finished capture takes that much longer to retire.
					static const int slowRetire = [] { const char* value = std::getenv("CCCP_TEST_SLOW_CAPTURE_RETIRE_MS"); return value ? std::atoi(value) : 0; }();
					if (slowRetire > 0) {
						static std::once_flag named;
						std::call_once(named, [] { System::PrintDiagnosticLine("[checkpoint-lever] slow_capture_retire_ms=" + std::to_string(slowRetire)); });
						std::this_thread::sleep_for(std::chrono::milliseconds(slowRetire));
					}
					// The image and all its fences retire before the next preparation.
					Publish(State::Preparing); continue;
				}
				// A boundary claimed meanwhile stops a warming or cooling run; the loop then finds it armed.
				if (state == State::Ready && m_PlanWarm != static_cast<int>(m_WantWarm.load(std::memory_order_acquire))) {
					const bool warm = m_WantWarm.load(std::memory_order_acquire);
					if (warm) {
						const auto began = RTE::SteadyClock::now();
						try { m_Plan->boundary->Warm(); } catch (...) {}
						m_LastWarmUs.store(std::chrono::duration_cast<std::chrono::microseconds>(RTE::SteadyClock::now() - began).count(), std::memory_order_release);
					}
					else m_Plan->boundary->Cool();
					m_PlanWarm = warm;
					m_Warmed.store(warm, std::memory_order_release);
					continue;
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
		struct Header { uint64_t tick; std::shared_ptr<const std::string> identity; };
		Header request{2, std::make_shared<const std::string>("first queued identity")};
		const std::weak_ptr<const std::string> firstIdentity = request.identity;
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
			       header.identity && *header.identity == (capture ? "second queued identity" : "first queued identity") &&
			       tvisnumber(&number) && numberVnum(&number) == (capture ? 23 : 17);
		});
		Release unblock{release};
		if (!queue.WaitUntilPrepared() || wrongThread.load(std::memory_order_acquire)) return "boundary inventory ran on the simulation thread";
		root.tick = 2; value->number = 13;
		ProtectedCall(lua, [&] { lua_pushinteger(lua, 17); lua_setglobal(lua, "queuedBoundaryValue"); });
		std::shared_future<bool> first, overlap;
		if (!queue.TryCapture(request, first)) return "prepared queued boundary refused";
		entered.get_future().wait();
		if (first.wait_for(std::chrono::seconds(0)) == std::future_status::ready || queue.TryCapture(request, overlap))
			return "queued boundary joined the saver or accepted an overlapping image";
		request.identity = std::make_shared<const std::string>("second queued identity");
		if (firstIdentity.expired()) return "queued boundary borrowed its request identity";
		root.tick = 3; value->number = 19;
		ProtectedCall(lua, [&] { lua_pushinteger(lua, 23); lua_setglobal(lua, "queuedBoundaryValue"); });
		unblock.Send();
		if (!first.get() || !queue.WaitUntilPrepared()) return "deferred boundary lost its frozen tick";
		if (!firstIdentity.expired()) return "queued boundary retained a completed request identity";
		const size_t growthBytes = std::async(std::launch::async, [&] { return owner->PrepareFreeze()->Image().ByteCount() + Snapshot::c_PageBytes; }).get();
		const std::string growth(growthBytes, 'x');
		ProtectedCall(lua, [&] { lua_pushlstring(lua, growth.data(), growth.size()); lua_setglobal(lua, "queuedGrowth"); });
		request.tick = 3;
		inject.store(true);
		if (queue.TryCapture(request, overlap)) return "queued boundary accepted a grown heap inventory";
		if (queue.WaitUntilPrepared() || !refused.load(std::memory_order_acquire)) return "queued inventory allocation refusal was not retained";
		queue.NotifyNextTick();
		if (!queue.WaitUntilPrepared()) return "queued inventory could not retry allocation refusal";
		bool filledOnSimulation = false;
		if (!queue.TryCapture(overlap, [&](Header& prepared) noexcept {
			filledOnSimulation = std::this_thread::get_id() == simulation;
			prepared.tick = request.tick; prepared.identity = request.identity;
		})) return "retried queued boundary refused";
		if (!filledOnSimulation) return "queued request did not copy at its simulation boundary";
		request.identity.reset();
		root.tick = 4; value.reset();
		if (!overlap.get()) return "retried queued boundary did not survive source destruction";
		{
			CheckpointFailure::Scope failure(CheckpointFailure::Point::CopyWatch, 1);
			const auto token = CheckpointFailure::Capture();
			const bool refused = std::async(std::launch::async, [token] {
				CheckpointFailure::ForwardScope forward(token);
				if (CheckpointFailure::Fails(CheckpointFailure::Point::CopyWatch)) return false;
				try { CheckpointFailure::Check(CheckpointFailure::Point::CopyWatch); }
				catch (const std::bad_alloc&) { return true; }
				return false;
			}).get();
			if (!refused || !failure.Triggered()) return "worker allocation refusal lost its capture count";
		}
		return CheckpointFailure::Current() == CheckpointFailure::Point::None ? std::string() : "worker refusal changed the next capture";
	}
}
