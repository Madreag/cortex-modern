#include "CheckpointArchive.h"
#include "CheckpointFailure.h"
#include "CheckpointFrozenContainers.h"
#include "TimerMan.h"
#include "FloatingPointEnvironment.h"

#include "Constants.h"
#include "PerformanceMan.h"
#include "SettingsMan.h"

#include <algorithm>
#include <limits>

using namespace RTE;

void TimerMan::PrepareCheckpointValues() {
	if (!CheckpointNativeStorage::Enabled() || m_OwnedCheckpointValues) return;
	CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
	auto prepared = std::make_unique<CheckpointState>(m_OrdinaryCheckpointValues);
	m_OwnedCheckpointValues = std::move(prepared);
	m_OrdinaryCheckpointValues.m_DeltaBuffer.clear();
}

std::string TimerMan::CheckpointStorageSelfTestMismatch() {
	CheckpointNativeStorage::AllocationScope allocations(true);
	CheckpointState ordinary{};
	ordinary.m_SimTimeTicks = 902;
	ordinary.m_SimUpdateCount = 60;
	ordinary.m_DeltaBuffer = {-0.0F, 8.25F};
	auto owned = std::make_unique<CheckpointState>(ordinary);
	const auto* source = owned.get();
	auto inventory = CheckpointNativeStorage::PrepareInventory();
	if (!inventory->Arm()) return "the owned timer inventory did not arm";
	owned->m_SimTimeTicks = 903;
	owned->m_SimUpdateCount = 61;
	owned->m_DeltaBuffer.clear();
	owned.reset();
	CheckpointNativeStorage::ReadScope read(inventory->Pages());
	const auto* saved = CheckpointNativeStorage::Source(source);
	const auto delta = CheckpointValues(saved->m_DeltaBuffer);
	return saved->m_SimTimeTicks == 902 && saved->m_SimUpdateCount == 60 && delta.size() == 2 &&
	    std::signbit(delta[0]) && delta[1] == 8.25F ? std::string() : "owned timer values changed after their source died";
}

void TimerMan::Clear() {
	CheckpointStateValues().m_StartTime = std::chrono::steady_clock::now();
	CheckpointStateValues().m_TicksPerSecond = 1000000;
	CheckpointStateValues().m_RealTimeTicks = 0;
	CheckpointStateValues().m_SimTimeTicks = 0;
	CheckpointStateValues().m_SimUpdateCount = 0;
	CheckpointStateValues().m_SimAccumulator = 0;
	CheckpointStateValues().m_DeltaTime = 0;
	CheckpointStateValues().m_DeltaTimeS = 0.0F;
	CheckpointStateValues().m_DeltaBuffer.clear();
	CheckpointStateValues().m_SimUpdatesSinceDrawn = -1;
	CheckpointStateValues().m_DrawnSimUpdate = false;
	CheckpointStateValues().m_SimFrameBudget = 0;
	CheckpointStateValues().m_SimFrameStart = 0;
	CheckpointStateValues().m_LastSimUpdateStart = 0;
	CheckpointStateValues().m_SimFrameBudgetSpent = false;
	CheckpointStateValues().m_SimHold = 0;
	CheckpointStateValues().m_OwedTicksKept = 0;
	CheckpointStateValues().m_OwedCapTickCostMS = 0;
	CheckpointStateValues().m_SimSpeed = 1.0F;
	CheckpointStateValues().m_TimeScale = 1.0F;
	CheckpointStateValues().m_SimPaused = false;
	CheckpointStateValues().m_SimTimeFrozen = false;
	CheckpointStateValues().m_FreeRunSim = false;
	CheckpointStateValues().m_PaceAccruedTicks = 0;
	CheckpointStateValues().m_PaceTrimmedTicks = 0;
	CheckpointStateValues().m_PaceWallSeenTicks = 0;
	CheckpointStateValues().m_PaceCapLostTicks = 0;
	CheckpointStateValues().m_PacePausedLostTicks = 0;
	CheckpointStateValues().m_PaceUpdateCalls = 0;
	CheckpointStateValues().m_PaceResetCalls = 0;
}

void TimerMan::Initialize() {
	// Get the frequency of ticks/s for this machine
	CheckpointStateValues().m_TicksPerSecond = 1000000;

	ResetTime();
	if (CheckpointStateValues().m_DeltaTimeS <= 0) {
		SetDeltaTimeSecs(c_DefaultDeltaTimeS);
	}
}

long long TimerMan::GetAbsoluteTime() const {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float TimerMan::GetRealToSimCap() const {
	return c_RealToSimCap;
}

float TimerMan::GetSimUpdateProportion() const {
	// An interpolation alpha: banked catch-up time past one dt must not extrapolate render
	// poses (or index past the trail buffers).
	return std::min(1.0F, CheckpointStateValues().m_SimAccumulator / static_cast<float>(CheckpointStateValues().m_DeltaTime));
}

float TimerMan::GetAIDeltaTimeSecs() const {
	return CheckpointStateValues().m_DeltaTimeS * static_cast<float>(g_SettingsMan.GetAIUpdateInterval());
}

void TimerMan::ResetTime() {
	++CheckpointStateValues().m_PaceResetCalls;
	CheckpointStateValues().m_StartTime = std::chrono::steady_clock::now();

	CheckpointStateValues().m_RealTimeTicks = 0;
	CheckpointStateValues().m_SimAccumulator = 0;
	CheckpointStateValues().m_SimHold = 0;
	CheckpointStateValues().m_SimTimeTicks = 0;
	CheckpointStateValues().m_SimUpdateCount = 0;
	CheckpointStateValues().m_SimUpdatesSinceDrawn = -1;
	CheckpointStateValues().m_DrawnSimUpdate = false;
	CheckpointStateValues().m_TimeScale = 1.0F;
}

void TimerMan::BeginSimFrame(long long budgetTicks) {
	CheckpointStateValues().m_SimFrameBudget = budgetTicks;
	CheckpointStateValues().m_SimFrameStart = GetAbsoluteTime();
	CheckpointStateValues().m_LastSimUpdateStart = 0;
	CheckpointStateValues().m_SimFrameBudgetSpent = false;
}

void TimerMan::UpdateSim() {
	FloatingPointEnvironment::Enter();
	FloatingPointEnvironment::Assert("simulation tick entry");
	if (TimeForSimUpdate()) {
		// Owed ticks past the budget wait for the next frame; this one, expected to end past it, is drawn.
		bool budgetSpent = false;
		if (CheckpointStateValues().m_SimFrameBudget > 0) {
			// The frame's previous update is the estimate for this one; the first update of a frame always runs.
			const long long now = GetAbsoluteTime();
			const long long previousCost = CheckpointStateValues().m_LastSimUpdateStart != 0 ? now - CheckpointStateValues().m_LastSimUpdateStart : 0;
			CheckpointStateValues().m_LastSimUpdateStart = now;
			budgetSpent = previousCost > 0 && now - CheckpointStateValues().m_SimFrameStart + previousCost >= CheckpointStateValues().m_SimFrameBudget;
		}
		// Transfer ticks from the accumulator to the sim time ticks. A free-running sim outpaces
		// the accumulator, so never draw it below zero.
		if (CheckpointStateValues().m_SimAccumulator >= CheckpointStateValues().m_DeltaTime) {
			CheckpointStateValues().m_SimAccumulator -= CheckpointStateValues().m_DeltaTime;
		}
		// A frozen tick advances the update count but not sim time, so sim timers hold still.
		if (!CheckpointStateValues().m_SimTimeFrozen) {
			CheckpointStateValues().m_SimTimeTicks += CheckpointStateValues().m_DeltaTime;
		}

		++CheckpointStateValues().m_SimUpdateCount;
		++CheckpointStateValues().m_SimUpdatesSinceDrawn;

		// If after deducting the DeltaTime from the accumulator, there is not enough time for another DeltaTime, then flag this as the last sim update before the frame is drawn.
		CheckpointStateValues().m_DrawnSimUpdate = !TimeForSimUpdate() || budgetSpent;
		CheckpointStateValues().m_SimFrameBudgetSpent = budgetSpent && TimeForSimUpdate();
	} else {
		CheckpointStateValues().m_DrawnSimUpdate = true;
	}
}

void TimerMan::Update() {
	long long prevTime = CheckpointStateValues().m_RealTimeTicks;
	CheckpointStateValues().m_RealTimeTicks = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - CheckpointStateValues().m_StartTime).count();

	// Cap timeIncrease if too long (as when the app went out of focus), to c_RealToSimCap.
	long long timeIncrease = std::min(CheckpointStateValues().m_RealTimeTicks - prevTime, static_cast<long long>(c_RealToSimCap * CheckpointStateValues().m_TicksPerSecond));

	// A steady clock can read the same microsecond on two quick updates; only time running backwards is bad data.
	RTEAssert(timeIncrease >= 0, "It seems your CPU is giving bad timing data to the game, this is known to happen on some multi-core processors. This may be fixed by downloading the latest CPU drivers from AMD or Intel.");

	CheckpointStateValues().m_PaceWallSeenTicks += CheckpointStateValues().m_RealTimeTicks - prevTime;
	CheckpointStateValues().m_PaceCapLostTicks += (CheckpointStateValues().m_RealTimeTicks - prevTime) - timeIncrease;
	++CheckpointStateValues().m_PaceUpdateCalls;

	// If not paused, add the new time difference to the sim accumulator
	if (!CheckpointStateValues().m_SimPaused) {
		long long accrued = static_cast<long long>(static_cast<float>(timeIncrease) * CheckpointStateValues().m_TimeScale);
		// A held clock lets that much real time go by first.
		const long long held = std::min(CheckpointStateValues().m_SimHold, accrued);
		CheckpointStateValues().m_SimHold -= held;
		accrued -= held;
		CheckpointStateValues().m_SimAccumulator += accrued;
		CheckpointStateValues().m_PaceAccruedTicks += accrued;
	} else {
		CheckpointStateValues().m_PacePausedLostTicks += timeIncrease;
	}

	float maxPossibleSimSpeed = GetDeltaTimeMS() / std::max(g_PerformanceMan.GetMSPSUAverage(), std::numeric_limits<float>::epsilon());

	// Make sure we don't get runaway behind schedule
	// A lockstep machine keeps what it owes up to its floor, even when a spike (a capture, a busy core) inflates the estimate.
	const float capSimSpeed = CheckpointStateValues().m_OwedCapTickCostMS > 0 ? GetDeltaTimeMS() / CheckpointStateValues().m_OwedCapTickCostMS : maxPossibleSimSpeed;
	const long long trimCap = std::max(CheckpointStateValues().m_DeltaTime + static_cast<long long>(CheckpointStateValues().m_DeltaTime * capSimSpeed), CheckpointStateValues().m_DeltaTime * (1 + CheckpointStateValues().m_OwedTicksKept));
	if (CheckpointStateValues().m_SimAccumulator > trimCap) {
		CheckpointStateValues().m_PaceTrimmedTicks += CheckpointStateValues().m_SimAccumulator - trimCap;
		CheckpointStateValues().m_SimAccumulator = trimCap;
	}

	RTEAssert(CheckpointStateValues().m_SimAccumulator >= 0, "Negative sim time accumulator?!");

	// Reset the counter since the last drawn update. Set it negative since we're counting full pure sim updates and this will be incremented to 0 on next SimUpdate.
	if (CheckpointStateValues().m_DrawnSimUpdate) {
		CheckpointStateValues().m_SimUpdatesSinceDrawn = -1;
	}

	CheckpointStateValues().m_SimSpeed = std::min(maxPossibleSimSpeed, GetTimeScale());
}

std::string TimerMan::SaveCheckpoint() const {
	CheckpointWriter writer("TimerMan2");
	VisitCheckpoint(writer, CheckpointStateValues());
	return writer.Text();
}

bool TimerMan::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		// TimerMan1 wrote the same fields with the free-run flag in the shared state.
		const bool legacy = text.starts_with("9 TimerMan1 ");
		CheckpointReader reader(text, legacy ? "TimerMan1" : "TimerMan2", validateOnly);
		VisitCheckpoint(reader, CheckpointStateValues());
		reader.Finish();
		if (!validateOnly) CheckpointStateValues().m_StartTime = std::chrono::steady_clock::now() - std::chrono::microseconds(CheckpointStateValues().m_RealTimeTicks);
		return true;
	} catch (const std::exception&) {
		return false;
	}
}
