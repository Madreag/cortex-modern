#pragma once

namespace RTE {

	/// Walks every (seat state, event) pair of the rejoin machine in process and reports each pair's outcome against the table in docs/rejoin-matrix.md.
	class NetRejoinMatrixSelfTest {
	public:
		static int Run();
		/// Drives every reachable cell of REJOIN-GRID.md (a seat's phase x the event) and reports each cell.
		static int RunGrid();
	};

} // namespace RTE
