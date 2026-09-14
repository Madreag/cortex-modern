#include "SettingsMultiplayerGUI.h"
#include "SettingsMan.h"

#include "GUI.h"
#include "GUIButton.h"
#include "GUICollectionBox.h"
#include "GUITab.h"

using namespace RTE;

SettingsMultiplayerGUI::SettingsMultiplayerGUI(GUIControlManager* parentControlManager) :
    m_GUIControlManager(parentControlManager) {
	m_MultiplayerSettingsBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxMultiplayerSettings"));

	m_PageTabs[static_cast<int>(Page::Player)] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabMpPagePlayer"));
	m_PageTabs[static_cast<int>(Page::Chat)] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabMpPageChat"));
	m_PageTabs[static_cast<int>(Page::Recovery)] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabMpPageRecovery"));
	m_PageTabs[static_cast<int>(Page::Files)] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabMpPageFiles"));
	m_PageTabs[static_cast<int>(Page::Internet)] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabMpPageInternet"));

	m_PageBoxes[static_cast<int>(Page::Player)] = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxMpPagePlayer"));
	m_PageBoxes[static_cast<int>(Page::Chat)] = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxMpPageChat"));
	m_PageBoxes[static_cast<int>(Page::Recovery)] = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxMpPageRecovery"));
	m_PageBoxes[static_cast<int>(Page::Files)] = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxMpPageFiles"));
	m_PageBoxes[static_cast<int>(Page::Internet)] = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxMpPageInternet"));

	m_ApplyButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonMultiplayerApply"));

	SetActivePage(Page::Player);
}

void SettingsMultiplayerGUI::SetEnabled(bool enable) const {
	m_MultiplayerSettingsBox->SetVisible(enable);
	m_MultiplayerSettingsBox->SetEnabled(enable);
}

void SettingsMultiplayerGUI::SetActivePage(Page page) {
	m_ActivePage = page;
	for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
		m_PageBoxes[index]->SetVisible(index == static_cast<int>(page));
		m_PageTabs[index]->SetCheck(index == static_cast<int>(page));
	}
}

void SettingsMultiplayerGUI::HandleInputEvents(GUIEvent& guiEvent) {
	if (guiEvent.GetType() == GUIEvent::Notification && guiEvent.GetMsg() == GUITab::UnPushed) {
		for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
			if (guiEvent.GetControl() == m_PageTabs[index]) {
				SetActivePage(static_cast<Page>(index));
				return;
			}
		}
	}
}
