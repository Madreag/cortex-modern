#pragma once

/// @file
/// What the renderer actually drew. A readback that asks "is this panel on the screen?" reads this
/// record instead of a visible flag, which only says what the panel would draw if its parents did.

namespace RTE {

	/// Records that the renderer drew this panel.
	void RecordPanelDraw(const void* panel);

	/// Whether the renderer drew this panel within the given number of seconds.
	bool PanelDrewRecently(const void* panel, double seconds);

	/// Drops the record; the caller destroyed the panels it refers to.
	void ClearPanelDrawRecord();
} // namespace RTE
