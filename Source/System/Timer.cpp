#include "Timer.h"

RTE::Timer::Timer(const Timer& source, CheckpointNativeSnapshot&) :
	m_StartRealTime(source.m_StartRealTime), m_StartSimTime(source.m_StartSimTime),
	m_RealTimeLimit(source.m_RealTimeLimit), m_SimTimeLimit(source.m_SimTimeLimit), m_TicksPerMS(source.m_TicksPerMS) {}

using namespace RTE;

void Timer::Clear() {
	m_StartRealTime = g_TimerMan.GetRealTickCount();
	m_StartSimTime = g_TimerMan.GetSimTickCount();
	m_RealTimeLimit = -1;
	m_SimTimeLimit = -1;
}

int Timer::Create() {
	m_TicksPerMS = static_cast<double>(g_TimerMan.GetTicksPerSecond()) * 0.001;
	return 0;
}

int Timer::Create(double simTimeLimit, double elapsedSimTime) {
	m_TicksPerMS = static_cast<double>(g_TimerMan.GetTicksPerSecond()) * 0.001;
	SetSimTimeLimitMS(simTimeLimit);
	if (elapsedSimTime >= 0) {
		SetElapsedSimTimeMS(elapsedSimTime);
	}
	return 0;
}

int Timer::Create(const Timer& reference) {
	m_StartRealTime = reference.m_StartRealTime;
	m_StartSimTime = reference.m_StartSimTime;
	m_RealTimeLimit = reference.m_RealTimeLimit;
	m_SimTimeLimit = reference.m_SimTimeLimit;
	m_TicksPerMS = static_cast<double>(g_TimerMan.GetTicksPerSecond()) * 0.001;
	return 0;
}
