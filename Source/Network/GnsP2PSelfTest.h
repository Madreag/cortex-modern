#pragma once

#include <string>
#include <vector>

namespace RTE {

	class GnsP2PSelfTest {
	public:
		/// args: the words after -net-p2p-selftest; none runs the single-process connect.
		static int Run(const std::vector<std::string>& args);
	};

} // namespace RTE
