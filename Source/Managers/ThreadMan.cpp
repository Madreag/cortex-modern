#include "ThreadMan.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace RTE;

void SaverThreads::Enter() {
	if (s_Lowered) return;
	s_Current = s_Lowered = true;
	// Lowest leaves a waking saver below the simulation's threads even with the scheduler's wake boost.
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
	setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

BS::thread_pool& SaverThreads::Pool() {
	return g_ThreadMan.GetSaverThreadPool();
}

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
