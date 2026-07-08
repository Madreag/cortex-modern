#include "TimerMan.h"

#include "Constants.h"
#include "PerformanceMan.h"
#include "SettingsMan.h"

#include <algorithm>
#include <limits>

using namespace RTE;

void TimerMan::Clear() {
	m_StartTime = std::chrono::steady_clock::now();
	m_TicksPerSecond = 1000000;
	m_RealTimeTicks = 0;
	m_SimTimeTicks = 0;
	m_SimUpdateCount = 0;
	m_SimAccumulator = 0;
	m_DeltaTime = 0;
	m_DeltaTimeS = 0.0F;
	m_DeltaBuffer.clear();
	m_SimUpdatesSinceDrawn = -1;
	m_DrawnSimUpdate = false;
	m_SimSpeed = 1.0F;
	m_TimeScale = 1.0F;
	m_SimPaused = false;
	m_SimTimeFrozen = false;
	m_FreeRunSim = false;
	m_PaceAccruedTicks = 0;
	m_PaceTrimmedTicks = 0;
	m_PaceWallSeenTicks = 0;
	m_PaceCapLostTicks = 0;
	m_PacePausedLostTicks = 0;
	m_PaceUpdateCalls = 0;
	m_PaceResetCalls = 0;
}

void TimerMan::Initialize() {
	// Get the frequency of ticks/s for this machine
	m_TicksPerSecond = 1000000;

	ResetTime();
	if (m_DeltaTimeS <= 0) {
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
	return m_SimAccumulator / static_cast<float>(m_DeltaTime);
}

float TimerMan::GetAIDeltaTimeSecs() const {
	return m_DeltaTimeS * static_cast<float>(g_SettingsMan.GetAIUpdateInterval());
}

void TimerMan::ResetTime() {
	++m_PaceResetCalls;
	m_StartTime = std::chrono::steady_clock::now();

	m_RealTimeTicks = 0;
	m_SimAccumulator = 0;
	m_SimTimeTicks = 0;
	m_SimUpdateCount = 0;
	m_SimUpdatesSinceDrawn = -1;
	m_DrawnSimUpdate = false;
	m_TimeScale = 1.0F;
}

void TimerMan::UpdateSim() {
	if (TimeForSimUpdate()) {
		// Transfer ticks from the accumulator to the sim time ticks. A free-running sim outpaces
		// the accumulator, so never draw it below zero.
		if (m_SimAccumulator >= m_DeltaTime) {
			m_SimAccumulator -= m_DeltaTime;
		}
		// A frozen tick advances the update count but not sim time, so sim timers hold still.
		if (!m_SimTimeFrozen) {
			m_SimTimeTicks += m_DeltaTime;
		}

		++m_SimUpdateCount;
		++m_SimUpdatesSinceDrawn;

		// If after deducting the DeltaTime from the accumulator, there is not enough time for another DeltaTime, then flag this as the last sim update before the frame is drawn.
		m_DrawnSimUpdate = !TimeForSimUpdate();
	} else {
		m_DrawnSimUpdate = true;
	}
}

void TimerMan::Update() {
	long long prevTime = m_RealTimeTicks;
	m_RealTimeTicks = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_StartTime).count();

	// Cap timeIncrease if too long (as when the app went out of focus), to c_RealToSimCap.
	long long timeIncrease = std::min(m_RealTimeTicks - prevTime, static_cast<long long>(c_RealToSimCap * m_TicksPerSecond));

	RTEAssert(timeIncrease > 0, "It seems your CPU is giving bad timing data to the game, this is known to happen on some multi-core processors. This may be fixed by downloading the latest CPU drivers from AMD or Intel.");

	m_PaceWallSeenTicks += m_RealTimeTicks - prevTime;
	m_PaceCapLostTicks += (m_RealTimeTicks - prevTime) - timeIncrease;
	m_PaceLastDeltaTicks = m_RealTimeTicks - prevTime;
	++m_PaceUpdateCalls;

	// If not paused, add the new time difference to the sim accumulator
	if (!m_SimPaused) {
		m_SimAccumulator += static_cast<long long>(static_cast<float>(timeIncrease) * m_TimeScale);
		m_PaceAccruedTicks += static_cast<long long>(static_cast<float>(timeIncrease) * m_TimeScale);
	} else {
		m_PacePausedLostTicks += timeIncrease;
	}

	float maxPossibleSimSpeed = GetDeltaTimeMS() / std::max(g_PerformanceMan.GetMSPSUAverage(), std::numeric_limits<float>::epsilon());

	// Make sure we don't get runaway behind schedule
	const long long trimCap = m_DeltaTime + static_cast<long long>(m_DeltaTime * maxPossibleSimSpeed);
	if (m_SimAccumulator > trimCap) {
		m_PaceTrimmedTicks += m_SimAccumulator - trimCap;
		m_SimAccumulator = trimCap;
	}

	RTEAssert(m_SimAccumulator >= 0, "Negative sim time accumulator?!");

	// Reset the counter since the last drawn update. Set it negative since we're counting full pure sim updates and this will be incremented to 0 on next SimUpdate.
	if (m_DrawnSimUpdate) {
		m_SimUpdatesSinceDrawn = -1;
	}

	m_SimSpeed = std::min(maxPossibleSimSpeed, GetTimeScale());
}
