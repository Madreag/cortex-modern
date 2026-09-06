#pragma once

namespace RTE {

	/// CC_FAULT_INJECT=<name>[,<name>...] arms deliberate faults in the test mechanisms only, to prove the gates catch them.
	bool FaultInjected(const char* name);

} // namespace RTE
