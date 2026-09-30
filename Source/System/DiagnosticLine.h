#pragma once

#include "System.h"

#include <sstream>

namespace RTE {

	/// One diagnostic line, composed with stream inserts and written whole under the engine's print lock when its statement ends,
	/// so a line printed on one thread never lands inside another thread's.
	struct DiagnosticLine : std::ostringstream {
		~DiagnosticLine() { System::PrintDiagnosticLine(str()); }
	};
} // namespace RTE
