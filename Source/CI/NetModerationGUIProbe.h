#pragma once

#include <cstdint>

namespace RTE::NetModerationGUIProbe {
	/// Runs an opt-in input script through the ordinary SDL event queue.
	void BeforePoll();
	/// Checks the frame after the network panel has been drawn.
	void AfterDraw();
	/// Applies the script's sim-rate keys on the tick they name, before that tick reads them.
	void OnSimTick(uint64_t simUpdateCount);
}
