#pragma once

/// @file
/// What the renderer actually drew. A readback that asks "is this panel on the screen?" reads this
/// record instead of a visible flag, which only says what the panel would draw if its parents did.

namespace RTE {

	/// Starts a manager's draw pass; the panels recorded until the matching end belong to it.
	/// @param manager The manager whose panels are about to draw.
	/// @return The pass that was open before, to hand back to EndPanelDrawPass.
	const void* BeginPanelDrawPass(const void* manager);

	/// Ends the pass BeginPanelDrawPass opened and reopens the one it returned.
	/// @param previous What BeginPanelDrawPass returned.
	void EndPanelDrawPass(const void* previous);

	/// Records that the renderer drew this panel in the open pass.
	void RecordPanelDraw(const void* panel);

	/// Whether this panel is in the frame on screen: drawn in its manager's latest pass, and that pass is no older than the given seconds.
	/// A panel drawn outside any pass counts if it drew within the given seconds.
	bool PanelDrawnInLatestPass(const void* panel, double seconds);

	/// Milliseconds since the renderer last drew this panel, or -1 if it never did.
	double PanelDrawAgeMs(const void* panel);

	/// Drops the record; the caller destroyed the panels it refers to.
	void ClearPanelDrawRecord();
} // namespace RTE
