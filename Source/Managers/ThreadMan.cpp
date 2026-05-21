#include "ThreadMan.h"

#include <cstdlib>

using namespace RTE;

ThreadMan::ThreadMan() {
	Clear();
	Create();
}

ThreadMan::~ThreadMan() {
	Destroy();
}

void ThreadMan::Clear() {
	// CCCP_SIM_THREADS sizes the priority pool (and, in LuaMan, the threaded Lua-state
	// count) so the determinism check can run the sim the way a low-core machine does —
	// where the see-ray / threaded passes share a small, contended pool. No-op unset.
	if (const char* simThreads = std::getenv("CCCP_SIM_THREADS"); simThreads != nullptr && std::atoi(simThreads) > 0) {
		m_PriorityThreadPool.reset(static_cast<unsigned>(std::atoi(simThreads)));
	} else {
		m_PriorityThreadPool.reset();
	}
	m_BackgroundThreadPool.reset(std::thread::hardware_concurrency() / 2);
}

int ThreadMan::Create() {
	return 0;
}

void ThreadMan::Destroy() {
	Clear();
}
