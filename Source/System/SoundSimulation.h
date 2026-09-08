#pragma once

#include <cstdint>

namespace RTE {

// A physical output device is presentation state. Simulation sound operations have
// their own clock and random stream; local AI never changes the shared cohort.
enum class SoundExecutionDomain : uint8_t { Presentation, SharedSimulation, LocalSimulation };

struct SoundExecutionKey {
	SoundExecutionDomain domain = SoundExecutionDomain::Presentation;
	uint64_t objectUID = 0;
	uint64_t tick = 0;
	uint64_t phase = 0;
	uint64_t occurrence = 0;
	uint64_t ordinal = 0;
};

class SoundSimulationScope {
public:
	SoundSimulationScope(uint64_t objectUID, uint64_t phase, SoundExecutionDomain domain = SoundExecutionDomain::SharedSimulation, uint64_t occurrence = 0);
	~SoundSimulationScope();
	SoundSimulationScope(const SoundSimulationScope&) = delete;
	SoundSimulationScope& operator=(const SoundSimulationScope&) = delete;
	static SoundExecutionDomain Domain();
	static bool IsSimulation() { return Domain() != SoundExecutionDomain::Presentation; }
	static SoundExecutionKey CurrentKey();
	static SoundExecutionKey NextQueryKey();
	static SoundExecutionKey NextPlayKey();
	static int RandomNum(int minimum, int maximum);
	static float RandomNum(float minimum, float maximum);
private:
	static uint64_t Mix(uint64_t value);
	uint32_t Draw();
	SoundSimulationScope* m_Previous;
	SoundExecutionKey m_Key;
	uint64_t m_Seed = 0;
	uint64_t m_DrawOrdinal = 0;
	uint64_t m_QueryOrdinal = 0;
	uint64_t m_PlayOrdinal = 0;
	uint64_t m_ChildOrdinal = 0;
	uint64_t m_LocalChildOrdinal = 0;
	static thread_local SoundSimulationScope* s_Current;
};

} // namespace RTE
