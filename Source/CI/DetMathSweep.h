#pragma once

namespace RTE::DetMathSweep {
	/// Hashes every detmath function over a dense input grid and prints one hash per
	/// function. Identical output across platforms proves the vendored math matches.
	/// @return Process exit code (0 on success).
	int Run();
} // namespace RTE::DetMathSweep
