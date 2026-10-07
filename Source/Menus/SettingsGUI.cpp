#include "SettingsGUI.h"
#include "WindowMan.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "GUIInputWrapper.h"
#include "GUICollectionBox.h"
#include "GUIButton.h"
#include "GUITab.h"
#include "GUILabel.h"
#include "GUISound.h"
#include "MenuAutomation.h"
#include "System.h"

using namespace RTE;

SettingsGUI::SettingsGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput, bool createForPauseMenu) {
	m_GUIControlManager = std::make_unique<GUIControlManager>();
	RTEAssert(m_GUIControlManager->Create(guiScreen, guiInput, "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Failed to create GUI Control Manager and load it from Base.rte/GUIs/Skins/Menus/MainMenuSubMenuSkin.ini");
	m_GUIControlManager->Load(createForPauseMenu ? "Base.rte/GUIs/SettingsPauseGUI.ini" : "Base.rte/GUIs/SettingsGUI.ini");

	int rootBoxMaxWidth = g_WindowMan.FullyCoversAllDisplays() ? g_WindowMan.GetPrimaryWindowDisplayWidth() / g_WindowMan.GetResMultiplier() : g_WindowMan.GetResX();

	GUICollectionBox* rootBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("root"));
	rootBox->Resize(rootBoxMaxWidth, g_WindowMan.GetResY());

	m_SettingsTabberBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxSettingsBase"));
	// The dialog floats centred at every resolution; the 140 pin only ever described 640x360's layout.
	m_SettingsTabberBox->CenterInParent(true, true);

	m_BackToMainButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonBackToMainMenu"));
	m_BackToMainButton->SetPositionAbs((rootBox->GetWidth() - m_BackToMainButton->GetWidth()) / 2, m_SettingsTabberBox->GetYPos() + m_SettingsTabberBox->GetHeight() + 10);

	m_SettingsMenuTabs[SettingsMenuScreen::VideoSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabVideoSettings"));
	m_SettingsMenuTabs[SettingsMenuScreen::AudioSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabAudioSettings"));
	m_SettingsMenuTabs[SettingsMenuScreen::InputSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabInputSettings"));
	m_SettingsMenuTabs[SettingsMenuScreen::GameplaySettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabGameplaySettings"));
	m_SettingsMenuTabs[SettingsMenuScreen::MiscSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabMiscSettings"));
	// The pause menu's skin carries no network page: its preferences belong to a session that has not started yet.
	m_SettingsMenuTabs[SettingsMenuScreen::NetworkSettingsMenu] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabNetworkSettings"));

	m_VideoSettingsMenu = std::make_unique<SettingsVideoGUI>(m_GUIControlManager.get());
	m_AudioSettingsMenu = std::make_unique<SettingsAudioGUI>(m_GUIControlManager.get());
	m_InputSettingsMenu = std::make_unique<SettingsInputGUI>(m_GUIControlManager.get());
	m_GameplaySettingsMenu = std::make_unique<SettingsGameplayGUI>(m_GUIControlManager.get());
	m_MiscSettingsMenu = std::make_unique<SettingsMiscGUI>(m_GUIControlManager.get());
	if (m_SettingsMenuTabs[SettingsMenuScreen::NetworkSettingsMenu]) {
		// A menu file replaced by an older version's lacks some of the page's controls: the page is then left out, not built half.
		const std::vector<std::string> missing = SettingsNetworkGUI::MissingControls(m_GUIControlManager.get());
		if (missing.empty()) {
			m_NetworkSettingsMenu = std::make_unique<SettingsNetworkGUI>(m_GUIControlManager.get());
		} else {
			std::string named;
			for (size_t i = 0; i < missing.size() && i < 4; ++i) named += (i ? ", " : "") + missing[i];
			System::PrintDiagnosticLine("[menu] multiplayer settings are off: Base.rte/GUIs/SettingsGUI.ini lacks " + std::to_string(missing.size()) +
			                            " of its controls (" + named + (missing.size() > 4 ? ", ..." : "") + ")");
			m_SettingsMenuTabs[SettingsMenuScreen::NetworkSettingsMenu]->SetVisible(false);
			m_SettingsMenuTabs[SettingsMenuScreen::NetworkSettingsMenu] = nullptr;
			if (GUIControl* page = m_GUIControlManager->GetControl("CollectionBoxNetworkSettings")) page->SetVisible(false);
		}
	}
	if (!createForPauseMenu && !m_NetworkSettingsMenu) {
		// The pause menu never had the page; here its absence means the menu file is from an older version, said in one sentence.
		if (GUILabel* off = dynamic_cast<GUILabel*>(m_GUIControlManager->AddControl("LabelSettingsMultiplayerOff", "LABEL", nullptr, 0, 0, 360, 40))) {
			if (GUIFont* small = m_GUIControlManager->GetSkin()->GetFont("FontSmall.png")) off->SetFont(small);
			off->SetHAlignment(GUIFont::Centre);
			off->SetVAlignment(GUIFont::Top);
			off->SetText("Multiplayer settings are off: this game's menu files (Base.rte/GUIs) are from an older version. Reinstall the game, or remove the mod that replaced them.");
			const int width = std::min(360, rootBox->GetWidth() - 20);
			off->Resize(width, off->GetTextHeight() + 4);
			off->SetPositionAbs((rootBox->GetWidth() - width) / 2, std::max(2, m_SettingsTabberBox->GetYPos() - off->GetHeight() - 4));
		}
	}

	if (createForPauseMenu) {
		m_SettingsTabberBox->SetPositionAbs((rootBox->GetWidth() - m_SettingsTabberBox->GetWidth()) / 2, (rootBox->GetHeight() - m_SettingsTabberBox->GetHeight() - 30) / 2);
		m_BackToMainButton->SetPositionAbs((rootBox->GetWidth() - m_BackToMainButton->GetWidth()) / 2, m_SettingsTabberBox->GetYPos() + m_SettingsTabberBox->GetHeight() + 10);

		SetActiveSettingsMenuScreen(SettingsMenuScreen::GameplaySettingsMenu, false);
	} else {
		SetActiveSettingsMenuScreen(SettingsMenuScreen::VideoSettingsMenu, false);
	}
	m_SettingsMenuTabs[m_ActiveSettingsMenuScreen]->SetCheck(true);
}

SettingsGUI::~SettingsGUI() = default;

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
		if (settingsTabberTab) {
			settingsTabberTab->SetEnabled(!disable);
		}
	}
}

void SettingsGUI::SetActiveSettingsMenuScreen(SettingsMenuScreen activeMenu, bool playButtonPressSound) {
	m_VideoSettingsMenu->SetEnabled(false);
	m_AudioSettingsMenu->SetEnabled(false);
	m_InputSettingsMenu->SetEnabled(false);
	m_GameplaySettingsMenu->SetEnabled(false);
	m_MiscSettingsMenu->SetEnabled(false);
	if (m_NetworkSettingsMenu) {
		m_NetworkSettingsMenu->SetEnabled(false);
	}

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
		case SettingsMenuScreen::NetworkSettingsMenu:
			m_NetworkSettingsMenu->SetEnabled(true);
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
				} else if (m_NetworkSettingsMenu && guiEvent.GetControl() == m_SettingsMenuTabs[SettingsMenuScreen::NetworkSettingsMenu]) {
					SetActiveSettingsMenuScreen(SettingsMenuScreen::NetworkSettingsMenu);
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
			case SettingsMenuScreen::NetworkSettingsMenu:
				m_NetworkSettingsMenu->HandleInputEvents(guiEvent);
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
	std::string observation;
	return MenuAutomation::HandClick(m_GUIControlManager.get(), name, nullptr, observation);
}
