#pragma once

#include <cstdint>
#include <string>

namespace RTE {
	class NetLockstepCoordinator;
	struct NetLockstepReadyFrame;

	class NetLockstepSelfTest {
	public:
		static int Run();
		static int RunFirstStart();
		static int RunOrdering();
		static int RunHoldHeartbeat();
		static int RunAcceptance();
		static int RunAcceptanceSteady();
		/// The seat transition record and the gaps and judgement that read it, alone.
		static int RunSeatLog();
		/// When a seat's claims on its actors end, on every peer and in a replay, alone.
		static int RunReleasedClaims();
		static int RunReleasePaths();
		static int RunSeatSuccession();
		static int RunSeatAdmission();
		static int RunRecoveryAfterReclaim();
		static bool CheckSessionRecoveryGuard(unsigned arm, std::string* error);
		static bool CheckSeatControllerState(bool returned, NetLockstepCoordinator& first, const NetLockstepReadyFrame& firstFrame,
		    NetLockstepCoordinator& second, const NetLockstepReadyFrame& secondFrame, uint8_t peer, std::string* error);
	};

} // namespace RTE
