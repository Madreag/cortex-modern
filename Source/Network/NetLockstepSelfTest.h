#pragma once

namespace RTE {

	class NetLockstepSelfTest {
	public:
		static int Run();
		static int RunFirstStart();
		static int RunOrdering();
		static int RunHoldHeartbeat();
	};

} // namespace RTE
