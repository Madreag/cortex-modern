#pragma once

namespace RTE::NetModerationGUIProbe {
	/// Runs an opt-in input script through the ordinary SDL event queue.
	void BeforePoll();
	/// Checks the frame after the network panel has been drawn.
	void AfterDraw();
}
