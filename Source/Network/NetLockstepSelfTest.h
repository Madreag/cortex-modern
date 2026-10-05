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
		/// When a seat's claims on its actors end, on every peer and in a replay, alone.
		static int RunReleasedClaims();
		static int RunReleasePaths();
		static int RunSeatSuccession();
	};

} // namespace RTE
