#include "GUI.h"
#include "Timer.h"

#include <cassert>
#include <iostream>

using namespace RTE;

GUIManager::GUIManager(GUIInput* input) {
	m_Input = input;
	m_MouseEnabled = true;
	m_UseValidation = false;

	Clear();

	// Maximum time allowed between two clicks for a double click
	// In milliseconds
	// m_DoubleClickTime = GetDoubleClickTime(); // Use windows' system value
	m_DoubleClickTime = 500;
	// m_DoubleClickSize = GetSystemMetrics(SM_CXDOUBLECLK)/2; // Use windows' system value
	m_DoubleClickSize = 2;
	m_DoubleClickButtons = GUIPanel::MOUSE_NONE;

	m_pTimer = new Timer();
}

GUIManager::~GUIManager() {
	delete m_pTimer;
	m_pTimer = nullptr;
}

void GUIManager::Clear() {
	m_PanelList.clear();
	m_CapturedPanel = nullptr;
	m_MouseOverPanel = nullptr;
	m_FocusPanel = nullptr;
	m_MouseEnabled = true;
	m_OldMouseX = m_OldMouseY = 0;
	m_UniqueIDCount = 0;

	m_HoverTrack = false;
	m_HoverPanel = nullptr;
	m_HoverTime = 0;
	SetRect(&m_DoubleClickRect, 0, 0, 0, 0);

	// Double click times
	m_LastMouseDown[0] = -99999.0F;
	m_LastMouseDown[1] = -99999.0F;
	m_LastMouseDown[2] = -99999.0F;
}

void GUIManager::AddPanel(GUIPanel* panel) {
	if (panel) {
		int Z = 0;

		// Get the last panel in the list
		if (!m_PanelList.empty()) {
			const GUIPanel* p = m_PanelList.at(m_PanelList.size() - 1);
			Z = p->GetZPos() + 1;
		}

		// Setup the panel
		panel->Setup(this, Z);

		// Add the panel to the list
		m_PanelList.push_back(panel);
	}
}

void GUIManager::Update(bool ignoreKeyboardEvents) {
	m_Input->Update();

	// Mouse Events
	int i;
	int MouseX = 0;
	int MouseY = 0;
	int DeltaX;
	int DeltaY;
	int MouseButtons[3];
	int MouseStates[3];
	int MouseWheelChange = 0;
	int Released = GUIPanel::MOUSE_NONE;
	int Pushed = GUIPanel::MOUSE_NONE;
	int Buttons = GUIPanel::MOUSE_NONE;
	int Repeated = GUIPanel::MOUSE_NONE;
	int Modifier = GUIInput::ModNone;
	int Mod = GUIPanel::MODI_NONE;

	float CurTime = m_pTimer->GetElapsedRealTimeMS();

	// Build the modifier state
	Modifier = m_Input->GetModifier();
	if (Modifier & GUIInput::ModShift) {
		Mod |= GUIPanel::MODI_SHIFT;
	}
	if (Modifier & GUIInput::ModCtrl) {
		Mod |= GUIPanel::MODI_CTRL;
	}
	if (Modifier & GUIInput::ModAlt) {
		Mod |= GUIPanel::MODI_ALT;
	}
	if (Modifier & GUIInput::ModCommand) {
		Mod |= GUIPanel::MODI_COMMAND;
	}

	// Get the mouse data
	if (m_MouseEnabled) {
		m_Input->GetMousePosition(&MouseX, &MouseY);
		m_Input->GetMouseButtons(MouseButtons, MouseStates);
		MouseWheelChange = m_Input->GetMouseWheelChange();

		// Calculate mouse movement
		DeltaX = MouseX - m_OldMouseX;
		DeltaY = MouseY - m_OldMouseY;
		m_OldMouseX = MouseX;
		m_OldMouseY = MouseY;

		// Panels that have captured the mouse get the events over anything else
		// Regardless where the mouse currently is
		GUIPanel* CurPanel = m_CapturedPanel;

		// Find the lowest panel in the tree that the mouse is over
		if (!CurPanel) {
			CurPanel = FindTopPanel(MouseX, MouseY);
		}

		// Build the states
		for (i = 0; i < 3; i++) {
			if (MouseButtons[i] == GUIInput::Released)
				Released |= 1 << i;
			if (MouseButtons[i] == GUIInput::Pushed)
				Pushed |= 1 << i;
			if (MouseButtons[i] == GUIInput::Repeat)
				Repeated |= 1 << i;
			if (MouseStates[i] == GUIInput::Down)
				Buttons |= 1 << i;
		}

		// Mouse Up
		if (Released != GUIPanel::MOUSE_NONE && CurPanel) {
			CurPanel->OnMouseUp(MouseX, MouseY, Released, Mod);
		}

		// Double click (on the mouse up)
		if (Released != GUIPanel::MOUSE_NONE && m_DoubleClickButtons != GUIPanel::MOUSE_NONE) {
			if (CurPanel) {
				CurPanel->OnDoubleClick(MouseX, MouseY, m_DoubleClickButtons, Mod);
			}
			m_LastMouseDown[0] = m_LastMouseDown[1] = m_LastMouseDown[2] = -99999.0f;
		}

		// Mouse Down
		if (Pushed != GUIPanel::MOUSE_NONE) {
			// Double click settings
			m_DoubleClickButtons = GUIPanel::MOUSE_NONE;

			// Check for a double click
			for (i = 0; i < 3; i++) {
				if (Pushed & (1 << i)) {
					if (CurTime - m_LastMouseDown[i] < (float)(m_DoubleClickTime) && MouseInRect(&m_DoubleClickRect, MouseX, MouseY)) {
						m_DoubleClickButtons |= (1 << i);
					} else {
						// Setup the first click
						m_DoubleClickButtons = GUIPanel::MOUSE_NONE;
						m_LastMouseDown[i] = CurTime;
					}
				}
			}

			// Setup the double click rectangle
			if (m_DoubleClickButtons == GUIPanel::MOUSE_NONE) {
				SetRect(&m_DoubleClickRect, MouseX - m_DoubleClickSize, MouseY - m_DoubleClickSize, MouseX + m_DoubleClickSize, MouseY + m_DoubleClickSize);
			}

			// OnMouseDown event
			if (CurPanel) {
				CurPanel->OnMouseDown(MouseX, MouseY, Pushed, Mod);
			}
		}

		// Mouse move
		if ((DeltaX != 0 || DeltaY != 0) && CurPanel) {
			CurPanel->OnMouseMove(MouseX, MouseY, Buttons, Mod);
		}

		// Mouse Hover
		if (m_HoverTrack && m_HoverTime < CurTime) {
			// Disable it (panel will have to re-enable it if it wants to continue)
			m_HoverTrack = false;

			GUIPanel* hover = m_HoverPanel;
			if (hover && hover->PointInside(MouseX, MouseY) /*GetPanelID() == CurPanel->GetPanelID()*/) {
				hover->OnMouseHover(MouseX, MouseY, Buttons, Mod);
			}
		}

		// Mouse enter & leave
		bool Enter = false;
		bool Leave = false;
		if (!m_MouseOverPanel && CurPanel) {
			Enter = true;
		}
		if (!CurPanel && m_MouseOverPanel) {
			Leave = true;
		}
		if (m_MouseOverPanel && CurPanel && m_MouseOverPanel->GetPanelID() != CurPanel->GetPanelID()) {
			Enter = true;
			Leave = true;
		}

		// OnMouseEnter
		if (Enter && CurPanel) {
			CurPanel->OnMouseEnter(MouseX, MouseY, Buttons, Mod);
		}

		// OnMouseLeave
		if (Leave && m_MouseOverPanel) {
			m_MouseOverPanel->OnMouseLeave(MouseX, MouseY, Buttons, Mod);
		}

		if (MouseWheelChange && CurPanel) {
			CurPanel->OnMouseWheelChange(MouseX, MouseY, Mod, MouseWheelChange);
		}

		m_MouseOverPanel = CurPanel;
	}

	if (!ignoreKeyboardEvents) {
		// Keyboard Events
		uint8_t KeyboardBuffer[256];
		m_Input->GetKeyboard(KeyboardBuffer);

		// If we don't have a panel with focus, just ignore keyboard events
		if (!m_FocusPanel) {
			return;
		}
		// If the panel is not enabled, don't send it key events
		if (!m_FocusPanel->IsEnabled()) {
			return;
		}

		for (i = 1; i < 256; i++) {
			GUIPanel* focused = m_FocusPanel;
			if (!focused || !focused->IsEnabled()) {
				break;
			}
			switch (KeyboardBuffer[i]) {
				// KeyDown & KeyPress
				case GUIInput::Pushed:
					focused->OnKeyDown(i, Mod);
					if (m_FocusPanel == focused) {
						focused->OnKeyPress(i, Mod);
					}
					break;

					// KeyUp
				case GUIInput::Released:
					if (m_FocusPanel == focused) {
						focused->OnKeyUp(i, Mod);
					}
					break;

					// KeyPress
				case GUIInput::Repeat:
					if (m_FocusPanel == focused) {
						focused->OnKeyPress(i, Mod);
					}
					break;
				default:
					break;
			}
		}
		std::string_view textInput;
		if (m_FocusPanel && m_FocusPanel->IsEnabled() && m_Input->GetTextInput(textInput)) {
			m_FocusPanel->OnTextInput(textInput);
		}
	}
}

void GUIManager::Draw(GUIScreen* Screen) {
	// Go through drawing panels that are invalid
	std::vector<GUIPanel*>::iterator it;

	for (it = m_PanelList.begin(); it != m_PanelList.end(); it++) {
		GUIPanel* p = *it;

		// Draw the panel
		if ((!p->IsValid() || !m_UseValidation) && p->_GetVisible()) {
			p->Draw(Screen);
		}
	}
}

void GUIManager::CaptureMouse(GUIPanel* Panel) {
	assert(Panel);

	// Release any old capture
	ReleaseMouse();

	// Setup the new capture
	m_CapturedPanel = Panel;
	m_CapturedPanel->SetCaptureState(true);
}

void GUIManager::ReleaseMouse() {
	if (m_CapturedPanel) {
		m_CapturedPanel->SetCaptureState(false);
	}

	m_CapturedPanel = nullptr;
}

GUIPanel* GUIManager::FindBottomPanel(int X, int Y) {
	std::vector<GUIPanel*>::iterator it;

	for (it = m_PanelList.begin(); it != m_PanelList.end(); it++) {
		GUIPanel* P = *it;
		if (P) {
			GUIPanel* CurPanel = P->BottomPanelUnderPoint(X, Y);
			if (CurPanel) {
				return CurPanel;
			}
		}
	}
	// No panel found
	return nullptr;
}

GUIPanel* GUIManager::FindTopPanel(int X, int Y) {
	std::vector<GUIPanel*>::reverse_iterator it;

	for (it = m_PanelList.rbegin(); it != m_PanelList.rend(); it++) {
		GUIPanel* P = *it;
		if (P) {
			GUIPanel* CurPanel = P->TopPanelUnderPoint(X, Y);
			if (CurPanel) {
				return CurPanel;
			}
		}
	}
	// No panel found
	return nullptr;
}

int GUIManager::GetPanelID() {
	return m_UniqueIDCount++;
}

bool GUIManager::MouseInRect(const GUIRect* Rect, int X, int Y) {
	if (!Rect) {
		return false;
	}
	if (X >= Rect->left && X <= Rect->right && Y >= Rect->top && Y <= Rect->bottom) {
		return true;
	}
	return false;
}

void GUIManager::TrackMouseHover(GUIPanel* Pan, bool Enabled, int Delay) {
	assert(Pan);
	m_HoverTrack = Enabled;
	m_HoverPanel = Pan;
	if (m_HoverTrack) {
		m_HoverTime = m_pTimer->GetElapsedRealTimeMS() + ((float)Delay / 1000.0F);
	}
}

void GUIManager::SetFocus(GUIPanel* Pan) {
	// Send the LoseFocus event to the old panel (if there is one)
	if (m_FocusPanel) {
		m_FocusPanel->OnLoseFocus();
	}

	m_FocusPanel = Pan;

	// Send the GainFocus event to the new panel
	if (m_FocusPanel) {
		m_FocusPanel->OnGainFocus();
	}
}

bool GUIManager::RunComboKeyCommitSelfTest() {
	// A dropped list drops the focus from its OnKeyDown, the way GUIComboBox::CloseDropped does; a real
	// list panel needs a skin and a screen, and this runs before either exists.
	class KeyPanel final : public GUIPanel {
	public:
		explicit KeyPanel(GUIManager* manager) : GUIPanel(manager) {}
		void MoveFocusOnEnter(GUIPanel* to) {
			m_MoveFocus = true;
			m_MoveTo = to;
		}
		void OnKeyDown(int KeyCode, int Modifier) override {
			m_Downs++;
			if (m_MoveFocus && KeyCode == GUIInput::Key_Enter) {
				m_Manager->SetFocus(m_MoveTo);
			}
		}
		void OnKeyPress(int KeyCode, int Modifier) override { m_Presses++; }

		int m_Downs = 0;
		int m_Presses = 0;

	private:
		bool m_MoveFocus = false;
		GUIPanel* m_MoveTo = nullptr;
	};
	class KeyInput final : public GUIInput {
	public:
		KeyInput() : GUIInput(-1, false) {}
		void PushEnter() { m_KeyboardBuffer[Key_Enter] = Pushed; }
	};
	// The manager's Timer reads g_TimerMan, and this runs before main() builds the managers.
	if (!TimerMan::IsConstructed()) {
		TimerMan::Construct();
	}

	bool passed = true;
	const auto check = [&passed](const char* label, bool value) {
		passed = passed && value;
		std::cout << "[combo-key-selftest] " << (value ? "PASS " : "FAIL ") << label << std::endl;
	};
	{
		// Return on the dropped list leaves no focused panel.
		KeyInput input;
		GUIManager manager(&input);
		manager.EnableMouse(false);
		KeyPanel dropped(&manager);
		dropped.MoveFocusOnEnter(nullptr);
		manager.AddPanel(&dropped);
		dropped.SetFocus();
		input.PushEnter();
		manager.Update();
		check("cleared_focus_ends_the_key", dropped.m_Downs == 1 && dropped.m_Presses == 0 && !manager.GetFocusPanel());
	}
	{
		// The same key hands the focus to another panel.
		KeyInput input;
		GUIManager manager(&input);
		manager.EnableMouse(false);
		KeyPanel next(&manager);
		KeyPanel dropped(&manager);
		dropped.MoveFocusOnEnter(&next);
		manager.AddPanel(&next);
		manager.AddPanel(&dropped);
		dropped.SetFocus();
		input.PushEnter();
		manager.Update();
		check("moved_focus_spares_the_new_panel", dropped.m_Downs == 1 && dropped.m_Presses == 0 &&
		                                             next.m_Downs == 0 && next.m_Presses == 0 && manager.GetFocusPanel() == &next);
	}
	{
		// A panel that keeps the focus still gets the key press.
		KeyInput input;
		GUIManager manager(&input);
		manager.EnableMouse(false);
		KeyPanel holder(&manager);
		manager.AddPanel(&holder);
		holder.SetFocus();
		input.PushEnter();
		manager.Update();
		check("kept_focus_still_presses", holder.m_Downs == 1 && holder.m_Presses == 1 && manager.GetFocusPanel() == &holder);
	}
	std::cout << "[combo-key-selftest] " << (passed ? "PASS" : "FAIL") << std::endl;
	return passed;
}
