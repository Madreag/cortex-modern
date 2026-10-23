#pragma once

#include <string>

namespace RTE {
	class NetSessionPlaneSelfTest {
	public:
		static int Run();
		static bool CheckHeldHostControllers(std::string* error);
		static bool CheckOwnerHitch(unsigned arm, std::string* error);
		static bool CheckHostAdministration(unsigned arm, std::string* error);
		static bool CheckSceneLifecycle(const std::string& name, std::string* error);
	};
}
