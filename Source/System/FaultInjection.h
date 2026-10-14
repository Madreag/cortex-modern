#pragma once

namespace RTE {

	/// CC_FAULT_INJECT=<name>[,<name>...] (or CC_TEST_FAULT_INJECT, for runners that pass only test levers) arms deliberate
	/// faults in the test mechanisms only, to prove the gates catch them.
	bool FaultInjected(const char* name);
	/// Arms named faults for a self-test without relying on the process environment.
	void TestArmFaultInject(const char* names);

} // namespace RTE
