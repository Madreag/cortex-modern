#pragma once

/// @file
/// Hiding a collection box hides the box, not the controls inside it: GUIManager::Draw reads each
/// panel's own visible flag, so a child of a hidden box keeps drawing over whatever replaced it.
/// A screen that swaps panels takes the subtree's visibility with the box and gives it back.

#include "GUI.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace RTE {

	/// The saved visibility of the controls under the boxes one menu has hidden. It lives in the menu
	/// that owns those controls, so a rebuilt control manager starts with an empty record.
	class MenuPanelVisibility {
	public:
		/// Shows or hides a box together with everything inside it, restoring what each control had.
		void SetPresent(GUIControl* box, bool present) {
			if (!box) {
				return;
			}
			// A box inside a box that is away comes back with it: the change lands in that record.
			if (std::vector<std::pair<GUIControl*, bool>>* ancestor = HiddenAncestorRecord(box)) {
				for (auto& [child, visible]: *ancestor) {
					if (child == box) {
						visible = present;
					}
				}
				return;
			}
			const auto stored = m_Hidden.find(box);
			if (!present) {
				if (stored == m_Hidden.end()) {
					std::vector<std::pair<GUIControl*, bool>> subtree;
					Collect(box, subtree);
					for (auto& [child, visible]: subtree) {
						child->SetVisible(false);
					}
					m_Hidden.emplace(box, std::move(subtree));
				}
			} else if (stored != m_Hidden.end()) {
				for (auto& [child, visible]: stored->second) {
					child->SetVisible(visible);
				}
				m_Hidden.erase(stored);
			}
			box->SetVisible(present);
		}

		/// Drops the record; the caller rebuilt the controls it referred to.
		void Forget() { m_Hidden.clear(); }

	private:
		/// The record of the nearest box above this one that is away, if any.
		std::vector<std::pair<GUIControl*, bool>>* HiddenAncestorRecord(GUIControl* control) {
			for (GUIControl* node = control->GetParent(); node; node = node->GetParent()) {
				const auto stored = m_Hidden.find(node);
				if (stored != m_Hidden.end()) {
					return &stored->second;
				}
			}
			return nullptr;
		}

		static void Collect(GUIControl* control, std::vector<std::pair<GUIControl*, bool>>& out) {
			if (std::vector<GUIControl*>* children = control->GetChildren()) {
				for (GUIControl* child: *children) {
					if (!child) {
						continue;
					}
					out.emplace_back(child, child->GetVisible());
					Collect(child, out);
				}
			}
		}

		std::unordered_map<GUIControl*, std::vector<std::pair<GUIControl*, bool>>> m_Hidden;
	};
} // namespace RTE
