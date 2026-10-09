#include "ThreadMan.h"

using namespace RTE;

ThreadMan::ThreadMan() :
    m_PriorityThreadPool(0, FloatingPointEnvironment::Initialize, [] { FloatingPointEnvironment::Assert("priority task"); }),
    m_BackgroundThreadPool(0, FloatingPointEnvironment::Initialize, [] { FloatingPointEnvironment::Assert("background task"); }) {
	Clear();
	Create();
}

ThreadMan::~ThreadMan() {
	Destroy();
}

void ThreadMan::Clear() {
	m_PriorityThreadPool.reset();
	m_BackgroundThreadPool.reset(std::thread::hardware_concurrency() / 2);
}

int ThreadMan::Create() {
	return 0;
}

void ThreadMan::Destroy() {
	Clear();
}
