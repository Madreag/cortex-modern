#include "CaptureSentinel.h"

using namespace RTE;

CaptureSentinel::WorkerScope::~WorkerScope() {
	s_Task = m_Previous;
}
