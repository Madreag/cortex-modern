#pragma once

#include <string>

namespace RTE {
	class NetSessionPlaneSelfTest {
	public:
		static int Run();
		static bool CheckOwnerHitch(unsigned arm, std::string* error);
		static bool CheckHostAdministration(unsigned arm, std::string* error);
	};
}
