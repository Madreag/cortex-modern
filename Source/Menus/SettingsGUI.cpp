#include "SettingsGUI.h"
#include "WindowMan.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "GUIInputWrapper.h"
#include "GUICollectionBox.h"
#include "GUIButton.h"
#include "GUITab.h"
#include "GUILabel.h"
#include "GUITextBox.h"
#include "GUICheckbox.h"
#include "GUIRadioButton.h"
#include "GUIComboBox.h"
#include "FrameMan.h"
#include "NetMatchService.h"
#include "NetLobbySnapshot.h"
#include "UInputMan.h"
#include "TimerMan.h"
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

using namespace RTE;

SettingsGUI::SettingsGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput, bool createForPauseMenu) {
	m_GUIControlManager = std::make_unique<GUIControlManager>();
	RTEAssert(m_GUIControlManager->Create(guiScreen, guiInput, "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Failed to create GUI Control Manager and load it from Base.rte/GUIs/Skins/Menus/MainMenuSubMenuSkin.ini");
	m_GUIControlManager->Load(createForPauseMenu ? "Base.rte/GUIs/SettingsPauseGUI.ini" : "Base.rte/GUIs/SettingsGUI.ini");

	int rootBoxMaxWidth = g_WindowMan.FullyCoversAllDisplays() ? g_WindowMan.GetPrimaryWindowDisplayWidth() / g_WindowMan.GetResMultiplier() : g_WindowMan.GetResX();

	GUICollectionBox* rootBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("root"));
	rootBox->Resize(rootBoxMaxWidth, g_WindowMan.GetResY());

	m_SettingsTabberBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxSettingsBase"));
	m_SettingsTabberBox->SetPositionAbs((rootBox->GetWidth() - m_SettingsTabberBox->GetWidth()) / 2, 140);
	if (rootBox->GetHeight() < 540) {
		m_SettingsTabberBox->CenterInParent(true, true);
	}

	m_BackToMainButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonBackToMainMenu"));
	m_BackToMainButton->SetPositionAbs((rootBox->GetWidth() - m_BackToMainButton->GetWidth()) / 2, m_SettingsTabberBox->GetYPos() + m_SettingsTabberBox->GetHeight() + 10);

	m_SettingsMenuTabs[SettingsMenuScreen::VideoSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabVideoSettings"));
	m_SettingsMenuTabs[SettingsMenuScreen::AudioSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabAudioSettings"));
	m_SettingsMenuTabs[SettingsMenuScreen::InputSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabInputSettings"));
	m_SettingsMenuTabs[SettingsMenuScreen::GameplaySettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabGameplaySettings"));
	m_SettingsMenuTabs[SettingsMenuScreen::MiscSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabMiscSettings"));

	m_VideoSettingsMenu = std::make_unique<SettingsVideoGUI>(m_GUIControlManager.get());
	m_AudioSettingsMenu = std::make_unique<SettingsAudioGUI>(m_GUIControlManager.get());
	m_InputSettingsMenu = std::make_unique<SettingsInputGUI>(m_GUIControlManager.get());
	m_GameplaySettingsMenu = std::make_unique<SettingsGameplayGUI>(m_GUIControlManager.get());
	m_MiscSettingsMenu = std::make_unique<SettingsMiscGUI>(m_GUIControlManager.get());

	if (createForPauseMenu) {
		m_SettingsTabberBox->SetPositionAbs((rootBox->GetWidth() - m_SettingsTabberBox->GetWidth()) / 2, (rootBox->GetHeight() - m_SettingsTabberBox->GetHeight() - 30) / 2);
		m_BackToMainButton->SetPositionAbs((rootBox->GetWidth() - m_BackToMainButton->GetWidth()) / 2, m_SettingsTabberBox->GetYPos() + m_SettingsTabberBox->GetHeight() + 10);

		SetActiveSettingsMenuScreen(SettingsMenuScreen::GameplaySettingsMenu, false);
	} else {
		SetActiveSettingsMenuScreen(SettingsMenuScreen::VideoSettingsMenu, false);
	}
	m_SettingsMenuTabs[m_ActiveSettingsMenuScreen]->SetCheck(true);
}

GUICollectionBox* SettingsGUI::GetActiveDialogBox() const {
	GUICollectionBox* activeDialogBox = nullptr;
	switch (m_ActiveSettingsMenuScreen) {
		case SettingsMenuScreen::VideoSettingsMenu:
			activeDialogBox = m_VideoSettingsMenu->GetActiveDialogBox();
			break;
		case SettingsMenuScreen::InputSettingsMenu:
			activeDialogBox = m_InputSettingsMenu->GetActiveDialogBox();
			break;
		default:
			break;
	}
	DisableSettingsMenuNavigation(activeDialogBox);

	return activeDialogBox;
}

void SettingsGUI::CloseActiveDialogBox() const {
	switch (m_ActiveSettingsMenuScreen) {
		case SettingsMenuScreen::VideoSettingsMenu:
			m_VideoSettingsMenu->CloseActiveDialogBox();
			g_GUISound.BackButtonPressSound()->Play();
			break;
		case SettingsMenuScreen::InputSettingsMenu:
			m_InputSettingsMenu->CloseActiveDialogBox();
			g_GUISound.BackButtonPressSound()->Play();
			break;
		default:
			break;
	}
}

void SettingsGUI::DisableSettingsMenuNavigation(bool disable) const {
	m_BackToMainButton->SetEnabled(!disable);
	for (GUITab* settingsTabberTab: m_SettingsMenuTabs) {
		settingsTabberTab->SetEnabled(!disable);
	}
}

void SettingsGUI::SetActiveSettingsMenuScreen(SettingsMenuScreen activeMenu, bool playButtonPressSound) {
	m_VideoSettingsMenu->SetEnabled(false);
	m_AudioSettingsMenu->SetEnabled(false);
	m_InputSettingsMenu->SetEnabled(false);
	m_GameplaySettingsMenu->SetEnabled(false);
	m_MiscSettingsMenu->SetEnabled(false);

	switch (activeMenu) {
		case SettingsMenuScreen::VideoSettingsMenu:
			m_VideoSettingsMenu->SetEnabled(true);
			break;
		case SettingsMenuScreen::AudioSettingsMenu:
			m_AudioSettingsMenu->SetEnabled(true);
			break;
		case SettingsMenuScreen::InputSettingsMenu:
			m_InputSettingsMenu->SetEnabled(true);
			break;
		case SettingsMenuScreen::GameplaySettingsMenu:
			m_GameplaySettingsMenu->SetEnabled(true);
			break;
		case SettingsMenuScreen::MiscSettingsMenu:
			m_MiscSettingsMenu->SetEnabled(true);
			break;
		default:
			RTEAbort("Invalid settings menu passed to SettingsGUI::SetActiveSettingsMenuScreen!");
			break;
	}
	m_ActiveSettingsMenuScreen = activeMenu;
	// Remove focus so the tab hovered graphic is removed after being pressed, otherwise it remains stuck on the active tab.
	m_GUIControlManager->GetManager()->SetFocus(nullptr);

	if (playButtonPressSound) {
		g_GUISound.BackButtonPressSound()->Play();
	}
}

bool SettingsGUI::HandleInputEvents() {
	m_GUIControlManager->Update();

	GUIEvent guiEvent;
	while (m_GUIControlManager->GetEvent(&guiEvent)) {
		if (guiEvent.GetType() == GUIEvent::Command) {
			if (guiEvent.GetControl() == m_BackToMainButton) {
				RefreshActiveSettingsMenuScreen();
				return true;
			}
		} else if (guiEvent.GetType() == GUIEvent::Notification) {
			if ((guiEvent.GetMsg() == GUIButton::Focused) && dynamic_cast<GUIButton*>(guiEvent.GetControl()) || (guiEvent.GetMsg() == GUITab::Hovered && dynamic_cast<GUITab*>(guiEvent.GetControl()))) {
				g_GUISound.SelectionChangeSound()->Play();
			}

			if (guiEvent.GetMsg() == GUITab::UnPushed) {
				if (guiEvent.GetControl() == m_SettingsMenuTabs[SettingsMenuScreen::VideoSettingsMenu]) {
					SetActiveSettingsMenuScreen(SettingsMenuScreen::VideoSettingsMenu);
				} else if (guiEvent.GetControl() == m_SettingsMenuTabs[SettingsMenuScreen::AudioSettingsMenu]) {
					SetActiveSettingsMenuScreen(SettingsMenuScreen::AudioSettingsMenu);
				} else if (guiEvent.GetControl() == m_SettingsMenuTabs[SettingsMenuScreen::InputSettingsMenu]) {
					SetActiveSettingsMenuScreen(SettingsMenuScreen::InputSettingsMenu);
				} else if (guiEvent.GetControl() == m_SettingsMenuTabs[SettingsMenuScreen::GameplaySettingsMenu]) {
					SetActiveSettingsMenuScreen(SettingsMenuScreen::GameplaySettingsMenu);
				} else if (guiEvent.GetControl() == m_SettingsMenuTabs[SettingsMenuScreen::MiscSettingsMenu]) {
					SetActiveSettingsMenuScreen(SettingsMenuScreen::MiscSettingsMenu);
				}
			}
		}
		switch (m_ActiveSettingsMenuScreen) {
			case SettingsMenuScreen::VideoSettingsMenu:
				m_VideoSettingsMenu->HandleInputEvents(guiEvent);
				break;
			case SettingsMenuScreen::AudioSettingsMenu:
				m_AudioSettingsMenu->HandleInputEvents(guiEvent);
				break;
			case SettingsMenuScreen::InputSettingsMenu:
				m_InputSettingsMenu->HandleInputEvents(guiEvent);
				break;
			case SettingsMenuScreen::GameplaySettingsMenu:
				m_GameplaySettingsMenu->HandleInputEvents(guiEvent);
				break;
			case SettingsMenuScreen::MiscSettingsMenu:
				m_MiscSettingsMenu->HandleInputEvents(guiEvent);
				break;
			default:
				RTEAbort("Trying to handle input events for an invalid settings menu in SettingsGUI::HandleInputEvents!");
				break;
		}
	}
	// Manual input config sequence has to be updated outside the GUI event handling loop otherwise input capture doesn't work (loop only runs if event queue isn't empty).
	if (m_ActiveSettingsMenuScreen == SettingsMenuScreen::InputSettingsMenu) {
		if (m_InputSettingsMenu->InputMappingConfigIsConfiguringManually()) {
			m_InputSettingsMenu->HandleMappingConfigManualConfiguration();
		} else if (m_InputSettingsMenu->InputConfigWizardIsConfiguringManually()) {
			m_InputSettingsMenu->HandleConfigWizardManualConfiguration();
		} else if (m_InputSettingsMenu->InputConfigIsConfiguringDevice()) {
			m_InputSettingsMenu->HandleConfigDeviceMapping();
		}
	}
	return false;
}

void SettingsGUI::Draw() const {
	m_GUIControlManager->Draw();
	m_GUIControlManager->DrawMouse();
}

bool SettingsGUI::AutomationPostCommand(const std::string& name) {
	if (!MenuAutomation::Enabled(m_GUIControlManager->GetControl(name))) return false;
	auto* input = dynamic_cast<GUIInputWrapper*>(m_GUIControlManager->GetInput());
	return input && input->QueueAutomationCommand([this, name] { MenuAutomation::Click(m_GUIControlManager.get(), name); });
}

namespace RTE::MenuAutomation {
	using Json = nlohmann::json;
	using Rect = std::array<int, 4>;
	Rect Rectangle(GUIPanel* panel) {
		Rect rect{0, 0, g_WindowMan.GetResX(), g_WindowMan.GetResY()};
		if (panel) panel->GetRect(&rect[0], &rect[1], &rect[2], &rect[3]);
		return rect;
	}
	bool Visible(GUIControl* control) {
		if (!control || !control->GetPanel()) return false;
		for (auto* node = control->GetPanel(); node; node = node->GetParentPanel()) if (!node->_GetVisible()) return false;
		return true;
	}
	bool Enabled(GUIControl* control) {
		if (!Visible(control)) return false;
		for (auto* node = control->GetPanel(); node; node = node->GetParentPanel()) if (!node->_GetEnabled()) return false;
		return true;
	}
	bool Text(GUIControl* control, std::string& text) {
		if (auto* value = dynamic_cast<GUILabel*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIButton*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUITextBox*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUICheckbox*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIRadioButton*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUITab*>(control)) text = value->GetText();
		else if (auto* value = dynamic_cast<GUIComboBox*>(control)) text = value->GetSelectedItem() ? value->GetSelectedItem()->m_Name : value->GetText();
		else return false;
		return true;
	}
	void Click(GUIControlManager* manager, const std::string& name) {
		auto* control = manager->GetControl(name);
		if (!Enabled(control)) return;
		auto* panel = control->GetPanel();
		const auto r = Rectangle(panel);
		panel->OnMouseDown(r[0] + r[2] / 2, r[1] + r[3] / 2, GUIPanel::MOUSE_LEFT, 0);
		panel->OnMouseUp(r[0] + r[2] / 2, r[1] + r[3] / 2, GUIPanel::MOUSE_LEFT, 0);
	}
	bool Inside(const Rect& child, const Rect& parent) {
		return child[2] > 0 && child[3] > 0 && child[0] >= parent[0] && child[1] >= parent[1] &&
			child[0] + child[2] <= parent[0] + parent[2] && child[1] + child[3] <= parent[1] + parent[3];
	}
	bool TextFits(GUIControlManager* manager, GUIControl* control, std::string& observation) {
		std::string text;
		if (!Visible(control) || !Text(control, text)) return false;
		const auto rect = Rectangle(control->GetPanel());
		observation += " text=" + Json(text).dump() + " rect=" + Json(rect).dump();
		if (auto* label = dynamic_cast<GUILabel*>(control); label && !label->GetHorizontalOverflowScroll()) {
			observation += " height=" + std::to_string(label->GetTextHeight()) + " word_width=" + std::to_string(label->GetMaxWordWidth());
			return label->GetTextHeight() <= rect[3] && label->GetMaxWordWidth() <= rect[2];
		}
		std::string section = dynamic_cast<GUIButton*>(control) ? "Button_Up" : dynamic_cast<GUITab*>(control) ? "Tab" :
			dynamic_cast<GUICheckbox*>(control) ? "Checkbox" : dynamic_cast<GUIRadioButton*>(control) ? "RadioButton" :
			dynamic_cast<GUITextBox*>(control) || dynamic_cast<GUIComboBox*>(control) ? "TextBox" : "Label";
		std::string fontName;
		auto* skin = manager->GetSkin();
		if (!skin->GetValue(section, "Font", &fontName)) return false;
		auto* font = skin->GetFont(fontName);
		if (!font) return false;
		int kerning = 0, width = rect[2], height = rect[3];
		skin->GetValue(section, "FontKerning", &kerning);
		const int savedKerning = font->GetKerning();
		font->SetKerning(kerning);
		if (section == "Button_Up") {
			for (const auto* side : {"Left", "Right", "Top", "Bottom"}) {
				int border[4]{};
				skin->GetValue(section, side, border, 4);
				if (std::string_view(side) == "Left" || std::string_view(side) == "Right") width -= border[2]; else height -= border[3];
			}
			--width; --height;
		} else if (section == "Tab") { text = " " + text; width -= 4; }
		else if (section == "Checkbox" || section == "RadioButton") {
			int base[4]{}; skin->GetValue(section, "Base", base, 4);
			width -= base[2] + (section == "Checkbox" ? 2 : 0); text = " " + text;
		} else if (section == "TextBox") {
			int margin = 3, top = 0;
			skin->GetValue(section, "WidthMargin", &margin); skin->GetValue(section, "HeightMargin", &top);
			width -= 2 * margin; height -= top;
			// A combo box shows its selected item in a text panel the 17 pixel drop-down button covers.
			if (dynamic_cast<GUIComboBox*>(control)) width -= 17;
		}
		const int textWidth = font->CalculateWidth(text), textHeight = font->CalculateHeight(text);
		observation += " measured=" + Json({textWidth, textHeight}).dump() + " available=" + Json({width, height}).dump();
		const bool fits = textWidth <= width && textHeight <= height;
		font->SetKerning(savedKerning);
		return fits;
	}
	bool Handles(const std::string& command) {
		return command == "assert_visible" || command == "assert_focus" || command == "assert_rect_inside" || command == "assert_text_fits" ||
			command == "dump_host_options" || command == "dump_player_options" || command == "focus_next" || command == "focus_previous" || command == "key" || command == "pad";
	}
	bool Execute(GUIControlManager* manager, const std::string& screen, const std::string& command, std::istream& args, std::string& observation) {
		try {
			if (!manager) { observation = "no active control manager"; return false; }
			std::string name, argument, extra;
			args >> std::quoted(name) >> argument >> extra;
			if (!extra.empty()) { observation = "unexpected arguments"; return false; }
			auto* control = manager->GetControl(name);
			if (command == "key" || command == "pad") {
				auto* input = dynamic_cast<GUIInputWrapper*>(manager->GetInput());
				observation = name + " " + argument;
				return input && (argument == "down" || argument == "up") && input->QueueAutomationInput(command, name, argument == "down");
			}
			if (command == "focus_next" || command == "focus_previous") {
				if (!name.empty()) return false;
				std::vector<GUIControl*> controls;
				for (auto* item : *manager->GetControlList()) if (Enabled(item) && !item->IsContainer() && !dynamic_cast<GUILabel*>(item)) controls.push_back(item);
				std::stable_sort(controls.begin(), controls.end(), [](auto* a, auto* b) {
					const auto x = Rectangle(a->GetPanel()), y = Rectangle(b->GetPanel());
					return std::tie(x[1], x[0]) < std::tie(y[1], y[0]);
				});
				if (controls.empty()) return false;
				const auto current = std::find_if(controls.begin(), controls.end(), [](auto* item) { return item->GetPanel()->HasFocus(); });
				const int count = static_cast<int>(controls.size()), index = static_cast<int>(current - controls.begin());
				const int next = current == controls.end() ? (command == "focus_next" ? 0 : count - 1) : (index + (command == "focus_next" ? 1 : count - 1)) % count;
				auto* target = controls[next];
				manager->GetManager()->SetFocus(target->GetPanel());
				const auto r = Rectangle(target->GetPanel());
				g_UInputMan.SetAbsoluteMousePosition(Vector(r[0] + r[2] / 2, r[1] + r[3] / 2) * g_WindowMan.GetResMultiplier());
				observation = target->GetName();
				return true;
			}
			if (command == "dump_host_options" || command == "dump_player_options") {
				if (!name.empty()) return false;
				static unsigned int capture = 0;
				const auto path = std::filesystem::path("ScreenShots") / (command + "_" + std::to_string(capture++));
				const auto lobby = g_NetMatchService.GetLobbySnapshot();
				Json result = {{"schema", 1}, {"screen", screen}, {"viewport", Rectangle(nullptr)}, {"service", lobby.serviceState},
					{"phase", "after_draw"}, {"sim_frame", g_TimerMan.GetSimUpdateCount()}, {"host", lobby.isHost}, {"peer_id", lobby.localPeerId}, {"controls", Json::array()}};
				for (auto* item : *manager->GetControlList()) {
					if (!Visible(item)) continue;
					auto* panel = item->GetPanel();
					auto* parent = dynamic_cast<GUIControl*>(panel->GetParentPanel());
					std::string text; Text(item, text);
					result["controls"].push_back({{"name", item->GetName()}, {"rect", Rectangle(panel)}, {"parent", parent ? parent->GetName() : ""},
						{"parent_rect", Rectangle(panel->GetParentPanel())}, {"text", text}, {"enabled", Enabled(item)}, {"visible", true}, {"focus", panel->HasFocus()}});
				}
				std::ofstream output(path.string() + ".json");
				output << result.dump(2) << '\n';
				output.flush();
				observation = result.dump();
				return output.good() && g_FrameMan.SaveBitmapToPNG(g_FrameMan.GetBackBuffer32(), (path.string() + ".png").c_str()) == 0;
			}
			observation = name + " " + argument;
			if (!control) { observation += " missing control"; return false; }
			if (command == "assert_visible") {
				observation += " actual=" + std::to_string(Visible(control));
				return (argument == "0" || argument == "1") && Visible(control) == (argument == "1");
			}
			if (command == "assert_focus") {
				observation += " actual=" + std::to_string(Visible(control) && control->GetPanel()->HasFocus());
				return argument.empty() && Visible(control) && control->GetPanel()->HasFocus();
			}
			if (command == "assert_text_fits") return argument.empty() && TextFits(manager, control, observation);
			if (command == "assert_rect_inside") {
				auto* parent = argument == "parent" ? control->GetPanel()->GetParentPanel() : manager->GetControl(argument) ? manager->GetControl(argument)->GetPanel() : nullptr;
				observation += " rect=" + Json(Rectangle(control->GetPanel())).dump() + " bounds=" + Json(Rectangle(parent)).dump();
				return (parent || argument == "viewport") && Inside(Rectangle(control->GetPanel()), Rectangle(parent));
			}
			return false;
		} catch (const std::exception& error) { observation = error.what(); return false; }
	}
}
