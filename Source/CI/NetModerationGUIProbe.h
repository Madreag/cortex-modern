#pragma once

#include <cstdint>

namespace RTE::NetModerationGUIProbe {
	/// Runs an opt-in input script through the ordinary SDL event queue.
	void BeforePoll();
	/// Checks the frame after the network panel has been drawn.
	void AfterDraw();
	/// Checks the frame after the menus have been drawn. The menu loop draws no network UI, so only the
	/// active menu's own steps are judged here.
	void AfterMenuDraw();
	/// Applies the script's sim-rate keys on the tick they name, before that tick reads them.
	void OnSimTick(uint64_t simUpdateCount);

	/// How many scripted rendezvous points (a written signal, a satisfied wait_file) this run has
	/// passed. A hold that waits on another peer's probe ends at one of these, so a watchdog that
	/// times the wait can count from the last one instead of from the start of the hold.
	uint64_t RendezvousCount();
}
