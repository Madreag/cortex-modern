#pragma once

namespace RTE {

	class NetMatchSelfTest {
	public:
		/// Checks fresh settings before the singleton managers are constructed.
		static int RunBeforeInitialization();
		static int Run();
		static int RunLobbyLifecycle();
	};

} // namespace RTE
