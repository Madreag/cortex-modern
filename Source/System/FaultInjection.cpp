#include "FaultInjection.h"

#include <cstdlib>
#include <string>

namespace RTE {

	bool FaultInjected(const char* name) {
		static const std::string armed = [] {
			const char* env = std::getenv("CC_FAULT_INJECT");
			return std::string(env ? env : "");
		}();
		if (armed.empty()) {
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

} // namespace RTE
