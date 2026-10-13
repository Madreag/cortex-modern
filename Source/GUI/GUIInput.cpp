#include "CheckpointArchive.h"
#include "GUI.h"
#include "WindowMan.h"
#include "SDL3/SDL.h"

#include <array>
#include <memory>

using namespace RTE;

bool GUIInput::m_OverrideInput = false;

int GUIInput::m_NetworkMouseButtonsEvents[4][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}};
int GUIInput::m_NetworkMouseButtonsStates[4][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}};
int GUIInput::m_PrevNetworkMouseButtonsStates[4][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}};

int GUIInput::m_NetworkMouseX[4] = {0, 0, 0, 0};
int GUIInput::m_NetworkMouseY[4] = {0, 0, 0, 0};

GUIInput::GUIInput(int whichPlayer, bool keyJoyMouseCursor) {
	// Clear all the states
	memset(m_KeyboardBuffer, 0, sizeof(unsigned char) * KEYBOARD_BUFFER_SIZE);
	memset(m_ScanCodeState, 0, sizeof(unsigned char) * KEYBOARD_BUFFER_SIZE);
	memset(m_MouseButtonsEvents, 0, sizeof(int) * 3);
	memset(m_MouseButtonsStates, 0, sizeof(int) * 3);

	m_TextInput.clear();
	m_HasTextInput = false;

	// memset(m_NetworkMouseButtonsEvents, -1, sizeof(int) * 3);
	// memset(m_NetworkMouseButtonsStates, -1, sizeof(int) * 3);

	m_MouseX = 0;
	m_MouseY = 0;
	m_LastFrameMouseX = 0;
	m_LastFrameMouseY = 0;

	m_MouseOffsetX = 0;
	m_MouseOffsetY = 0;
	m_Modifier = ModNone;

	m_KeyJoyMouseCursor = keyJoyMouseCursor;

	m_Player = whichPlayer;

	m_MouseWheelChange = 0;
}

GUIInput::~GUIInput() {
	Destroy();
}

void GUIInput::Destroy() {}

void GUIInput::GetKeyboard(unsigned char* Buffer) const {
	if (Buffer) {
		memcpy(Buffer, m_KeyboardBuffer, sizeof(unsigned char) * KEYBOARD_BUFFER_SIZE);
	}
}

unsigned char GUIInput::GetAsciiState(unsigned char ascii) const {
	return m_KeyboardBuffer[ascii];
}

unsigned char GUIInput::GetScanCodeState(unsigned char scancode) const {
	return m_ScanCodeState[scancode];
}

void GUIInput::GetMouseButtons(int* Buttons, int* States) const {
	if (!m_OverrideInput) {
		if (Buttons) {
			memcpy(Buttons, m_MouseButtonsEvents, sizeof(int) * 3);
		}
		if (States) {
			memcpy(States, m_MouseButtonsStates, sizeof(int) * 3);
		}
	} else {
		for (int i = 0; i < 3; i++) {
			Buttons[i] = -1;
			States[i] = -1;
		}
		if (m_Player >= 0 && m_Player < 4) {
			for (int b = 0; b < 3; b++) {
				Buttons[b] = m_NetworkMouseButtonsEvents[m_Player][b];
				States[b] = m_NetworkMouseButtonsStates[m_Player][b];
			}
		} else {
			for (int b = 0; b < 3; b++) {
				Buttons[b] = m_NetworkMouseButtonsEvents[0][b];
				States[b] = m_NetworkMouseButtonsStates[0][b];
			}
		}
	}
}

void GUIInput::ClearMouseState() {
	memset(m_MouseButtonsEvents, 0, sizeof(int) * 3);
	memset(m_MouseButtonsStates, 0, sizeof(int) * 3);
}

void GUIInput::SetNetworkMouseButton(int whichPlayer, int state1, int state2, int state3) {
	if (whichPlayer >= 0 && whichPlayer < 4) {
		m_OverrideInput = true;

		m_PrevNetworkMouseButtonsStates[whichPlayer][0] = m_NetworkMouseButtonsStates[whichPlayer][0];
		m_PrevNetworkMouseButtonsStates[whichPlayer][1] = m_NetworkMouseButtonsStates[whichPlayer][1];
		m_PrevNetworkMouseButtonsStates[whichPlayer][2] = m_NetworkMouseButtonsStates[whichPlayer][2];

		m_NetworkMouseButtonsStates[whichPlayer][0] = state1;
		m_NetworkMouseButtonsStates[whichPlayer][1] = state2;
		m_NetworkMouseButtonsStates[whichPlayer][2] = state3;
	}
}

void GUIInput::GetMousePosition(int* X, int* Y) const {
	if (m_OverrideInput) {
		if (m_Player >= 0 && m_Player < 4) {
			if (X) {
				*X = (m_NetworkMouseX[m_Player] + m_MouseOffsetX);
			}
			if (Y) {
				*Y = (m_NetworkMouseY[m_Player] + m_MouseOffsetY);
			}
		} else {
			if (X) {
				*X = (m_NetworkMouseX[0] + m_MouseOffsetX);
			}
			if (Y) {
				*Y = (m_NetworkMouseY[0] + m_MouseOffsetY);
			}
		}
	} else {
		if (X) {
			*X = (m_MouseX + m_MouseOffsetX);
		}
		if (Y) {
			*Y = (m_MouseY + m_MouseOffsetY);
		}
	}
}

void GUIInput::SetNetworkMouseMovement(int whichPlayer, int x, int y) {
	if (whichPlayer >= 0 && whichPlayer < 4) {
		m_OverrideInput = true;
		m_NetworkMouseX[whichPlayer] += x;
		m_NetworkMouseY[whichPlayer] += y;
	}
}

void GUIInput::Update() {
	// Do nothing
}

int GUIInput::GetModifier() const {
	return m_Modifier;
}

void GUIInput::StartTextInput() {
	m_TextInputActive++;
}

void GUIInput::StopTextInput() {
	m_TextInputActive--;
	if (m_TextInputActive < 0) {
		m_TextInputActive = 0;
	}
}

std::string GUIInput::SaveCheckpoint() const {
	CheckpointWriter writer("GUIInput1");
	VisitCheckpoint(writer, *this);
	return writer.Text();
}

bool GUIInput::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		CheckpointReader reader(text, "GUIInput1", validateOnly);
		VisitCheckpoint(reader, *this);
		reader.Finish();
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

namespace {
	struct SavedTextInput : CheckpointNativeAllocated { bool active = false; SDL_Rect area{}; int cursor = 0; };
	std::unique_ptr<SavedTextInput> s_SavedTextInput;
}

std::array<std::pair<const void*, size_t>, 6> GUIInput::CheckpointRootSources() {
	return {{{&m_OverrideInput, sizeof(m_OverrideInput)}, {m_NetworkMouseButtonsEvents, sizeof(m_NetworkMouseButtonsEvents)},
	    {m_NetworkMouseButtonsStates, sizeof(m_NetworkMouseButtonsStates)}, {m_PrevNetworkMouseButtonsStates, sizeof(m_PrevNetworkMouseButtonsStates)},
	    {m_NetworkMouseX, sizeof(m_NetworkMouseX)}, {m_NetworkMouseY, sizeof(m_NetworkMouseY)}}};
}

void GUIInput::PrepareCheckpointTextInput() {
	if (!CheckpointNativeStorage::Enabled() || s_SavedTextInput) return;
	s_SavedTextInput = std::make_unique<SavedTextInput>();
	UpdateCheckpointTextInput();
}

void GUIInput::UpdateCheckpointTextInput() {
	if (!s_SavedTextInput || CheckpointNativeStorage::Reading()) return;
	SavedTextInput values;
	if (auto* window = g_WindowMan.GetWindow()) {
		if (!SDL_GetTextInputArea(window, &values.area, &values.cursor)) throw std::runtime_error("could not read GUI text input area");
		values.active = SDL_TextInputActive(window);
	}
	*s_SavedTextInput = values;
}

std::string GUIInput::SaveSharedCheckpoint() {
	CheckpointWriter writer("GUISharedInput2");
	writer(*CheckpointNativeStorage::Source(&m_OverrideInput), *CheckpointNativeStorage::Source(&m_NetworkMouseButtonsEvents), *CheckpointNativeStorage::Source(&m_NetworkMouseButtonsStates),
		*CheckpointNativeStorage::Source(&m_PrevNetworkMouseButtonsStates), *CheckpointNativeStorage::Source(&m_NetworkMouseX), *CheckpointNativeStorage::Source(&m_NetworkMouseY));
	if (CheckpointNativeStorage::Reading()) {
		if (!s_SavedTextInput) throw std::logic_error("the saved GUI has no text input values");
		const auto* values = CheckpointNativeStorage::Source(s_SavedTextInput.get());
		writer(values->active, values->area.x, values->area.y, values->area.w, values->area.h, values->cursor);
		return writer.Text();
	}
	SDL_Rect area{};
	int cursor = 0;
	auto* window = g_WindowMan.GetWindow();
	if (window && !SDL_GetTextInputArea(window, &area, &cursor)) throw std::runtime_error("could not read GUI text input area");
	writer(window && SDL_TextInputActive(window), area.x, area.y, area.w, area.h, cursor);
	return writer.Text();
}

bool GUIInput::LoadSharedCheckpoint(std::string_view text, bool validateOnly) {
	try {
		const bool legacy = text.starts_with("15 GUISharedInput1 ");
		CheckpointReader reader(text, legacy ? "GUISharedInput1" : "GUISharedInput2", validateOnly);
		reader(m_OverrideInput, m_NetworkMouseButtonsEvents, m_NetworkMouseButtonsStates,
			m_PrevNetworkMouseButtonsStates, m_NetworkMouseX, m_NetworkMouseY);
		if (!legacy) {
			bool active;
			SDL_Rect area{};
			int cursor;
			reader.Value(active); reader.Value(area.x); reader.Value(area.y); reader.Value(area.w); reader.Value(area.h); reader.Value(cursor);
			if (area.w < 0 || area.h < 0) return false;
			reader.OnCommit([active, area, cursor] {
				if (auto* window = g_WindowMan.GetWindow()) {
					if (!SDL_SetTextInputArea(window, &area, cursor) || !(active ? SDL_StartTextInput(window) : SDL_StopTextInput(window)))
						throw std::runtime_error("could not restore GUI text input state");
				} else if (active) throw std::runtime_error("a GUI text input window is missing");
			});
		}
		reader.Finish();
		return true;
	} catch (const std::exception&) { return false; }
}
