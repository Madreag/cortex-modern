#pragma once

namespace RTE {

	class NetLockstepSelfTest {
	public:
		static int Run();
		static int RunFirstStart();
		static int RunOrdering();
		static int RunHoldHeartbeat();
		/// The seat transition record and the gaps and judgement that read it, alone.
		static int RunSeatLog();
	};

} // namespace RTE
