#include "GUI.h"
#include "CheckpointArchive.h"
#include "GUIInputWrapper.h"
#include "SDL3/SDL.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "UInputMan.h"
#include "Timer.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace RTE;

namespace {
	bool automationDriving = false;
	// The menu script and the net UI probe share one virtual pad; the hint they need is process-global.
	int joystickBackgroundEventHolders = 0;
	int scriptedPadHolders = 0;
	std::string savedJoystickHint;
	bool savedJoystickHintPresent = false;
	SDL_Joystick* scriptedPad = nullptr;
	SDL_Gamepad* scriptedPadGamepad = nullptr;
	std::vector<GUIInputWrapper*> automationInputs;

	bool EnsureScriptedPad() {
		if (scriptedPad) return true;
		GUIInputWrapper::AcquireJoystickBackgroundEvents();
		SDL_VirtualJoystickDesc desc{};
		SDL_INIT_INTERFACE(&desc);
		desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
		desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
		desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
		desc.button_mask = (1U << SDL_GAMEPAD_BUTTON_COUNT) - 1;
		desc.axis_mask = (1U << SDL_GAMEPAD_AXIS_COUNT) - 1;
		desc.name = "Menu script controller";
		const auto id = SDL_AttachVirtualJoystick(&desc);
		if (!id) {
			GUIInputWrapper::ReleaseJoystickBackgroundEvents();
			return false;
		}
		// Claimed before the added event arrives, so no seat's control scheme ever binds this pad.
		UInputMan::RegisterScriptedPad(id);
		scriptedPad = SDL_OpenJoystick(id);
		if (!scriptedPad) {
			UInputMan::ForgetScriptedPad(id);
			SDL_DetachVirtualJoystick(id);
			GUIInputWrapper::ReleaseJoystickBackgroundEvents();
			return false;
		}
		scriptedPadGamepad = SDL_OpenGamepad(id);
		return true;
	}

	class ScriptedGUIInput final : public GUIInputWrapper {
		std::array<bool, SDL_SCANCODE_COUNT> m_Keys{};
		std::function<void()> m_Command;
		GUIInputWrapper* m_Physical;
		bool m_HoldsPad = false;
	public:
		ScriptedGUIInput(GUIInputWrapper* physical, int player) : GUIInputWrapper(player, physical->GetKeyJoyMouseCursor()), m_Physical(physical) { automationInputs.push_back(this); }
		~ScriptedGUIInput() override { ReleaseAutomationInput(); std::erase(automationInputs, this); }
		void Update() override {
			SetKeyJoyMouseCursor(m_Physical->GetKeyJoyMouseCursor());
			int count = 0;
			const bool* physical = SDL_GetKeyboardState(&count);
			std::array<bool, SDL_SCANCODE_COUNT> merged{};
			for (int i = 0; i < count && i < SDL_SCANCODE_COUNT; ++i) merged[i] = physical[i] || m_Keys[i];
			UpdateWithKeyboard(merged.data());
			if (m_Keys[SDL_SCANCODE_LSHIFT] || m_Keys[SDL_SCANCODE_RSHIFT]) m_Modifier |= ModShift;
			if (m_Keys[SDL_SCANCODE_LCTRL] || m_Keys[SDL_SCANCODE_RCTRL]) m_Modifier |= ModCtrl;
			if (m_Keys[SDL_SCANCODE_LALT] || m_Keys[SDL_SCANCODE_RALT]) m_Modifier |= ModAlt;
			if (m_Keys[SDL_SCANCODE_LGUI] || m_Keys[SDL_SCANCODE_RGUI]) m_Modifier |= ModCommand;
			if (auto command = std::exchange(m_Command, {})) command();
		}
		bool QueueAutomationCommand(std::function<void()> command) override {
			if (!automationDriving) return false;
			m_Command = std::move(command);
			return true;
		}
		bool QueueAutomationInput(const std::string& device, const std::string& name, bool down) override {
			if (!automationDriving) return false;
			if (device == "key") {
				const SDL_Scancode key = SDL_GetScancodeFromName(name == "KP1" ? "Keypad 1" : name.c_str());
				if (key == SDL_SCANCODE_UNKNOWN) return false;
				SDL_Event event{};
				event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
				event.key.windowID = SDL_GetWindowID(g_WindowMan.GetWindow());
				event.key.scancode = key;
				event.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
				event.key.down = down;
				int count = 0;
				if ((down || !SDL_GetKeyboardState(&count)[key]) && !SDL_PushEvent(&event)) return false;
				m_Keys[key] = down;
				return true;
			}
			if (device != "pad") return false;
			if (!m_HoldsPad) {
				if (!GUIInputWrapper::AcquireScriptedPad()) return false;
				m_HoldsPad = true;
			}
			return GUIInputWrapper::QueueScriptedPad(name, down);
		}
		void ReleaseAutomationInput() override {
			m_Command = {};
			for (int key = 1; key < SDL_SCANCODE_COUNT; ++key) {
				if (m_Keys[key]) QueueAutomationInput("key", SDL_GetScancodeName(static_cast<SDL_Scancode>(key)), false);
			}
			m_Keys.fill(false);
			if (m_HoldsPad) {
				GUIInputWrapper::ReleaseScriptedPad();
				m_HoldsPad = false;
			}
		}
	};
}

void GUIInputWrapper::SetAutomationDriving(bool enabled) {
	if (!enabled) for (auto* input : automationInputs) input->ReleaseAutomationInput();
	automationDriving = enabled;
}

void GUIInputWrapper::AcquireJoystickBackgroundEvents() {
	if (joystickBackgroundEventHolders++ == 0) {
		const char* previous = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
		savedJoystickHintPresent = previous != nullptr;
		savedJoystickHint = previous ? previous : "";
		SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	}
}

void GUIInputWrapper::ReleaseJoystickBackgroundEvents() {
	if (joystickBackgroundEventHolders > 0 && --joystickBackgroundEventHolders == 0) {
		if (savedJoystickHintPresent) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, savedJoystickHint.c_str());
		else SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
	}
}

bool GUIInputWrapper::AcquireScriptedPad() {
	if (!EnsureScriptedPad()) return false;
	++scriptedPadHolders;
	return true;
}

bool GUIInputWrapper::QueueScriptedPad(const std::string& name, bool down) {
	const std::string mapped = name == "south" ? "a" : name == "east" ? "b" : name == "west" ? "x" : name == "north" ? "y" : name;
	const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(mapped.c_str());
	if (button == SDL_GAMEPAD_BUTTON_INVALID) return false;
	if (!EnsureScriptedPad()) return false;
	if (!SDL_SetJoystickVirtualButton(scriptedPad, button, down)) return false;
	SDL_UpdateJoysticks();
	return true;
}

void GUIInputWrapper::ReleaseScriptedPad() {
	if (scriptedPadHolders > 0) --scriptedPadHolders;
	if (scriptedPadHolders > 0 || !scriptedPad) return;
	const auto id = SDL_GetJoystickID(scriptedPad);
	if (scriptedPadGamepad) {
		SDL_CloseGamepad(scriptedPadGamepad);
		scriptedPadGamepad = nullptr;
	}
	SDL_CloseJoystick(scriptedPad);
	SDL_DetachVirtualJoystick(id);
	UInputMan::ForgetScriptedPad(id);
	scriptedPad = nullptr;
	GUIInputWrapper::ReleaseJoystickBackgroundEvents();
}

uint32_t GUIInputWrapper::ScriptedPadId() {
	return scriptedPad ? static_cast<uint32_t>(SDL_GetJoystickID(scriptedPad)) : 0;
}

std::unique_ptr<GUIInputWrapper> GUIInputWrapper::CreateAutomationInput() {
	return automationDriving ? std::make_unique<ScriptedGUIInput>(this, m_Player) : nullptr;
}

std::string GUIInputWrapper::SaveCheckpoint() const {
	CheckpointWriter writer("GUIInputWrapper1");
	writer(GUIInput::SaveCheckpoint(), m_KeyHoldDuration, *m_KeyTimer, *m_CursorAccelTimer);
	return writer.Text();
}

bool GUIInputWrapper::LoadCheckpoint(std::string_view text, bool validateOnly) {
	if (text.starts_with("9 GUIInput1 ")) return GUIInput::LoadCheckpoint(text, validateOnly);
	try {
		CheckpointReader reader(text, "GUIInputWrapper1", validateOnly);
		std::string base;
		reader.Value(base);
		if (!GUIInput::LoadCheckpoint(base, true)) return false;
		reader(m_KeyHoldDuration, *m_KeyTimer, *m_CursorAccelTimer);
		reader.OnCommit([this, base = std::move(base)] { GUIInput::LoadCheckpoint(base); });
		reader.Finish();
		return true;
	} catch (const std::exception&) { return false; }
}

GUIInputWrapper::GUIInputWrapper(int whichPlayer, bool keyJoyMouseCursor) :
	GUIInput(whichPlayer, keyJoyMouseCursor),
	m_KeyTimer(std::make_unique<Timer>()),
	m_CursorAccelTimer(std::make_unique<Timer>())
	{

	memset(m_KeyboardBuffer, 0, sizeof(uint8_t) * GUIInput::Constants::KEYBOARD_BUFFER_SIZE);
	memset(m_ScanCodeState, 0, sizeof(uint8_t) * GUIInput::Constants::KEYBOARD_BUFFER_SIZE);

	m_KeyHoldDuration.fill(-1);
}

void GUIInputWrapper::ConvertKeyEvent(bool down, int guilibKey, float elapsedS) {
	if (down) {
		if (m_KeyHoldDuration[guilibKey] < 0) {
			m_KeyboardBuffer[guilibKey] = GUIInput::Pushed;
			m_KeyHoldDuration[guilibKey] = 0;
		} else if (m_KeyHoldDuration[guilibKey] < m_KeyRepeatDelay) {
			m_KeyboardBuffer[guilibKey] = GUIInput::None;
		} else {
			m_KeyboardBuffer[guilibKey] = GUIInput::Repeat;
			m_KeyHoldDuration[guilibKey] = 0;
		}
		m_KeyHoldDuration[guilibKey] += elapsedS;
	} else {
		if (m_KeyHoldDuration[guilibKey] >= 0) {
			m_KeyboardBuffer[guilibKey] = GUIInput::Released;
		} else {
			m_KeyboardBuffer[guilibKey] = GUIInput::None;
		}
		m_KeyHoldDuration[guilibKey] = -1;
	}
}

void GUIInputWrapper::Update() {
	int count = 0;
	UpdateWithKeyboard(SDL_GetKeyboardState(&count));
}

void GUIInputWrapper::UpdateWithKeyboard(const bool* keys) {
	float keyElapsedTime = static_cast<float>(m_KeyTimer->GetElapsedRealTimeS());
	m_KeyTimer->Reset();

	UpdateKeyboardInput(keyElapsedTime, keys);
	UpdateMouseInput();

	// If joysticks and keyboard can control the mouse cursor too.
	if (m_KeyJoyMouseCursor) {
		UpdateKeyJoyMouseInput(keyElapsedTime);
	}

	// Update the mouse position of this GUIInput, based on the SDL mouse vars (which may have been altered by joystick or keyboard input).
	Vector mousePos = g_UInputMan.GetAbsoluteMousePosition(m_Player);
	m_MouseX = static_cast<int>(mousePos.GetX() / static_cast<float>(g_WindowMan.GetResMultiplier()));
	m_MouseY = static_cast<int>(mousePos.GetY() / static_cast<float>(g_WindowMan.GetResMultiplier()));
}

void GUIInputWrapper::StartTextInput() {
	GUIInput::StartTextInput();
	SDL_StartTextInput(g_WindowMan.GetWindow());
}

void GUIInputWrapper::StopTextInput() {
	GUIInput::StopTextInput();
	if (m_TextInputActive <= 0) {
		SDL_StopTextInput(g_WindowMan.GetWindow());
	}
}

void GUIInputWrapper::UpdateKeyboardInput(float keyElapsedTime, const bool* keys) {
	// Clear the keyboard buffer, we need it to check for changes.
	memset(m_KeyboardBuffer, 0, sizeof(uint8_t) * GUIInput::Constants::KEYBOARD_BUFFER_SIZE);
	memset(m_ScanCodeState, 0, sizeof(uint8_t) * GUIInput::Constants::KEYBOARD_BUFFER_SIZE);

	for (size_t k = 0; k < GUIInput::Constants::KEYBOARD_BUFFER_SIZE; ++k) {
		if (g_UInputMan.KeyPressed(static_cast<SDL_Scancode>(k))) {
			m_ScanCodeState[k] = GUIInput::Pushed;
			uint8_t keyName = static_cast<uint8_t>(SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(k), NULL, false));
			m_KeyboardBuffer[keyName] = GUIInput::Pushed;
		}
	}
	m_HasTextInput = g_UInputMan.GetTextInput(m_TextInput);

	ConvertKeyEvent(keys[SDL_SCANCODE_SPACE], ' ', keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_BACKSPACE], GUIInput::Key_Backspace, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_TAB], GUIInput::Key_Tab, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_RETURN], GUIInput::Key_Enter, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_KP_ENTER], GUIInput::Key_Enter, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_ESCAPE], GUIInput::Key_Escape, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_LEFT], GUIInput::Key_LeftArrow, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_RIGHT], GUIInput::Key_RightArrow, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_UP], GUIInput::Key_UpArrow, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_DOWN], GUIInput::Key_DownArrow, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_INSERT], GUIInput::Key_Insert, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_DELETE], GUIInput::Key_Delete, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_HOME], GUIInput::Key_Home, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_END], GUIInput::Key_End, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_PAGEUP], GUIInput::Key_PageUp, keyElapsedTime);
	ConvertKeyEvent(keys[SDL_SCANCODE_PAGEDOWN], GUIInput::Key_PageDown, keyElapsedTime);

	m_Modifier = GUIInput::ModNone;
	SDL_Keymod keyShifts = SDL_GetModState();

	if (keyShifts & SDL_KMOD_SHIFT) {
		m_Modifier |= GUIInput::ModShift;
	}
	if (keyShifts & SDL_KMOD_ALT) {
		m_Modifier |= GUIInput::ModAlt;
	}
	if (keyShifts & SDL_KMOD_CTRL) {
		m_Modifier |= GUIInput::ModCtrl;
	}
	if (keyShifts & SDL_KMOD_GUI) {
		m_Modifier |= GUIInput::ModCommand;
	}
}

void GUIInputWrapper::UpdateMouseInput() {
	const auto& buttonStates = g_UInputMan.GetMouseState(m_Player);
	const auto& buttonChange = g_UInputMan.GetMouseChange(m_Player);
	Vector mousePos = g_UInputMan.GetAbsoluteMousePosition(m_Player);

	m_MouseX = mousePos.GetFloorIntX();
	m_MouseY = mousePos.GetFloorIntY();

	for (int button = 0; button < 3; button++) {
		// GUI runs from sim-tick context in-activity (BuyMenuGUI::Update etc.); the render-rate
		// change[] gets cleared by EndFrame between iters, eating clicks that arrive on render-only
		// iters. OR-in the sim-rate accumulators (which survive across render frames until the next
		// sim tick consumes via EndSimUpdate) so the GUI sees the event either way.
		bool changed = buttonChange[button + 1] || g_UInputMan.MouseButtonPressedSim(button + 1, m_Player) || g_UInputMan.MouseButtonReleasedSim(button + 1, m_Player);
		m_MouseButtonsStates[button] = buttonStates[button + 1] ? Down : Up;
		if (m_MouseButtonsStates[button] == Down) {
			m_MouseButtonsEvents[button] = changed ? Pushed : Repeat;
		} else {
			m_MouseButtonsEvents[button] = changed ? Released : None;
		}
	}
}

void GUIInputWrapper::UpdateKeyJoyMouseInput(float keyElapsedTime) {
	// TODO Try to not use magic numbers throughout this method.
	float mouseDenominator = g_WindowMan.GetResMultiplier();
	Vector joyKeyDirectional = g_UInputMan.GetMenuDirectional() * 5;

	// See how much to accelerate the joystick input based on how long the stick has been pushed around.
	if (joyKeyDirectional.MagnitudeIsLessThan(0.95F)) {
		m_CursorAccelTimer->Reset();
	}

	float acceleration = 0.25F + static_cast<float>(std::min(m_CursorAccelTimer->GetElapsedRealTimeS(), 0.5)) * 20.0F;
	Vector newMousePos = g_UInputMan.GetAbsoluteMousePosition(m_Player);

	// Manipulate the mouse position with the joysticks or keys.
	newMousePos.m_X += joyKeyDirectional.GetX() * static_cast<float>(mouseDenominator) * keyElapsedTime * 15.0F * acceleration;
	newMousePos.m_Y += joyKeyDirectional.GetY() * static_cast<float>(mouseDenominator) * keyElapsedTime * 15.0F * acceleration;

	// Keep the mouse within the bounds of the screen. Give it a bit of leeway on the right side, to account for the cursor sprite size.
	newMousePos.m_X = std::clamp(newMousePos.m_X, 0.0F, static_cast<float>(g_WindowMan.GetResX() * mouseDenominator) - 3.0F);
	newMousePos.m_Y = std::clamp(newMousePos.m_Y, 0.0F, static_cast<float>(g_WindowMan.GetResY() * mouseDenominator) - 3.0F);

	g_UInputMan.SetAbsoluteMousePosition(newMousePos, m_Player);

	// Update mouse button states and presses. In the menu, either left or mouse button works.
	if (g_UInputMan.MenuButtonHeld(UInputMan::MenuCursorButtons::MENU_EITHER)) {
		m_MouseButtonsStates[0] = GUIInput::Down;
		m_MouseButtonsEvents[0] = GUIInput::Repeat;
	}
	if (g_UInputMan.MenuButtonPressed(UInputMan::MenuCursorButtons::MENU_EITHER)) {
		m_MouseButtonsStates[0] = GUIInput::Down;
		m_MouseButtonsEvents[0] = GUIInput::Pushed;
	} else if (g_UInputMan.MenuButtonReleased(UInputMan::MenuCursorButtons::MENU_EITHER)) {
		m_MouseButtonsStates[0] = GUIInput::Up;
		m_MouseButtonsEvents[0] = GUIInput::Released;
	} else if (m_MouseButtonsEvents[0] == GUIInput::Released) {
		m_MouseButtonsStates[0] = GUIInput::Up;
		m_MouseButtonsEvents[0] = GUIInput::None;
	}

    m_MouseWheelChange = g_UInputMan.MouseWheelMovedByPlayer(m_Player);
}
