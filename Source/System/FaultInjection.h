#pragma once

namespace RTE {

	/// CC_FAULT_INJECT=<name>[,<name>...] arms deliberate faults in the test mechanisms only, to prove the gates catch them.
	bool FaultInjected(const char* name);
	/// Arms named faults for a self-test without relying on the process environment.
	void TestArmFaultInject(const char* names);

} // namespace RTE
