#include "FaultInjection.h"

#include <cstdlib>
#include <initializer_list>
#include <string>

namespace RTE {

	namespace {
		std::string g_TestArmedFaults;

		bool FaultListHas(const std::string& armed, const char* name) {
			if (!name || armed.empty()) {
				return false;
			}
			size_t start = 0;
			while (start <= armed.size()) {
				const size_t comma = armed.find(',', start);
				const std::string item = armed.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
				if (item == name) {
					return true;
				}
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
			return false;
		}
	}

	void TestArmFaultInject(const char* names) {
		g_TestArmedFaults = names ? names : "";
	}

	bool FaultInjected(const char* name) {
		// CC_TEST_FAULT_INJECT carries the same list for runners that pass only test levers.
		static const std::string armed = [] {
			std::string names;
			for (const char* variable: {"CC_FAULT_INJECT", "CC_TEST_FAULT_INJECT"})
				if (const char* env = std::getenv(variable)) names += (names.empty() ? "" : ",") + std::string(env);
			return names;
		}();
		return FaultListHas(g_TestArmedFaults, name) || FaultListHas(armed, name);
	}

} // namespace RTE
