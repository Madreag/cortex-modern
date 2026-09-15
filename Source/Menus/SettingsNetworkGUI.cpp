#include "SettingsNetworkGUI.h"
#include "SettingsMan.h"
#include "NetMatchConfig.h"

#include "GUI.h"
#include "GUICollectionBox.h"
#include "GUICheckbox.h"
#include "GUIComboBox.h"
#include "GUILabel.h"
#include "GUIRadioButton.h"
#include "GUITab.h"
#include "GUITextBox.h"

#include <algorithm>
#include <charconv>

using namespace RTE;

namespace {
	// The page's row pitch, by which the rows under the fixed-delay row close up when it is away.
	constexpr int c_FixedDelayRowHeight = 20;

	// The selector row and the page boxes share their names with the page they switch in.
	constexpr std::array<const char*, 5> c_PageNames{"Player", "Chat", "Recovery", "Files", "Internet"};

	// The boxes take typed digits only, so anything else came from a skin edit and is discarded.
	bool ParseWholeNumber(const std::string& text, int& value) {
		const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
		return result.ec == std::errc() && result.ptr == text.data() + text.size();
	}
} // namespace

SettingsNetworkGUI::SettingsNetworkGUI(GUIControlManager* parentControlManager) :
    m_GUIControlManager(parentControlManager) {
	m_NetworkSettingsBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxNetworkSettings"));

	for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
		const std::string page = c_PageNames[index];
		m_PageTabs[index] = dynamic_cast<GUITab*>(m_GUIControlManager->GetControl("TabNetPage" + page));
		m_PageBoxes[index] = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxNetPage" + page));
	}

	m_DisplayNameTextbox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextNetworkDisplayName"));
	// The lobby's own name box takes 24 characters; both boxes write the same setting.
	m_DisplayNameTextbox->SetMaxTextLength(24);

	m_DelayPolicyAutoRadio = dynamic_cast<GUIRadioButton*>(m_GUIControlManager->GetControl("RadioNetworkDelayAuto"));
	m_DelayPolicyFixedRadio = dynamic_cast<GUIRadioButton*>(m_GUIControlManager->GetControl("RadioNetworkDelayFixed"));

	m_FixedDelayLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetworkFixedDelay"));
	m_FixedDelayTextbox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextNetworkFixedDelay"));
	m_FixedDelayTextbox->SetNumericOnly(true);
	m_FixedDelayTextbox->SetMaxTextLength(2);
	m_FixedDelayHintLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetworkFixedDelayHint"));
	m_FixedDelayHintLabel->SetText("frames, 0-" + std::to_string(NetMatchConfigUtil::c_MaxInputDelayFrames));

	m_IdleWaitLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetworkIdleWait"));
	m_IdleWaitHintLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetworkIdleWaitHint"));
	m_IdleWaitTextbox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextNetworkIdleWait"));
	m_IdleWaitTextbox->SetNumericOnly(true);
	m_IdleWaitTextbox->SetMaxTextLength(2);

	m_AutoRepairCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkAutoRepair"));
	m_ToastsCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkToasts"));
	m_PredictionCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkPrediction"));

	m_StatusModeCombo = dynamic_cast<GUIComboBox*>(m_GUIControlManager->GetControl("ComboMatchStatusWidget"));
	m_StatusModeCombo->AddItem("Off");
	m_StatusModeCombo->AddItem("Auto");
	m_StatusModeCombo->AddItem("Always");

	const auto rowTop = [](GUIControl* control) {
		int x = 0, y = 0, width = 0, height = 0;
		control->GetControlRect(&x, &y, &width, &height);
		return std::make_pair(control, y);
	};
	m_RowsUnderFixedDelay = {rowTop(m_IdleWaitLabel), rowTop(m_IdleWaitTextbox), rowTop(m_IdleWaitHintLabel), rowTop(m_AutoRepairCheckbox)};

	ShowSavedValues();
}

void SettingsNetworkGUI::SetEnabled(bool enable) {
	// Leaving the page commits the boxes, the same as pressing enter in one, so nothing typed is lost.
	if (!enable && m_NetworkSettingsBox->GetVisible()) {
		ApplyTextboxes();
	}
	m_NetworkSettingsBox->SetVisible(enable);
	m_NetworkSettingsBox->SetEnabled(enable);
	if (enable) {
		ShowSavedValues();
	}
}

void SettingsNetworkGUI::ShowSavedValues() {
	m_DisplayNameTextbox->SetText(g_SettingsMan.GetNetworkDisplayName());
	const bool fixed = g_SettingsMan.GetNetworkHostDelayPolicy() == SettingsMan::NetworkHostDelayPolicy::Fixed;
	m_DelayPolicyAutoRadio->SetCheck(!fixed);
	m_DelayPolicyFixedRadio->SetCheck(fixed);
	m_FixedDelayTextbox->SetText(std::to_string(std::clamp(g_SettingsMan.GetNetworkInputDelayFrames(), 0, static_cast<int>(NetMatchConfigUtil::c_MaxInputDelayFrames))));
	m_IdleWaitTextbox->SetText(std::to_string(g_SettingsMan.GetNetworkHostIdleWaitMinutes()));
	m_AutoRepairCheckbox->SetCheck(g_SettingsMan.GetNetworkHostAutoRepair());
	m_ToastsCheckbox->SetCheck(g_SettingsMan.GetNetworkToastsEnabled());
	m_PredictionCheckbox->SetCheck(g_SettingsMan.LocalPredictionEnabled());
	m_StatusModeCombo->SetSelectedIndex(static_cast<int>(g_SettingsMan.GetNetworkMatchStatusMode()));
	UpdateDelayPolicyRow();
	UpdateStatusLines();
}

void SettingsNetworkGUI::ApplyTextboxes() {
	g_SettingsMan.SetNetworkDisplayName(m_DisplayNameTextbox->GetText());
	if (int minutes = 0; ParseWholeNumber(m_IdleWaitTextbox->GetText(), minutes)) {
		g_SettingsMan.SetNetworkHostIdleWaitMinutes(minutes);
	}
	if (int frames = 0; ParseWholeNumber(m_FixedDelayTextbox->GetText(), frames)) {
		g_SettingsMan.SetNetworkInputDelayFrames(std::clamp(frames, 0, static_cast<int>(NetMatchConfigUtil::c_MaxInputDelayFrames)));
	}
	// A refused value never stays on screen: the settings are what the page states.
	ShowSavedValues();
	m_NetworkSettingsBox->SetFocus();
}

void SettingsNetworkGUI::UpdateDelayPolicyRow() {
	// The video page hides the resolution rows its radio does not apply to; the same cue here, because
	// a label has no disabled look and a bright caption over a dim box reads as an error.
	const bool fixed = g_SettingsMan.GetNetworkHostDelayPolicy() == SettingsMan::NetworkHostDelayPolicy::Fixed;
	GUIControl* const row[] = {m_FixedDelayLabel, m_FixedDelayTextbox, m_FixedDelayHintLabel};
	for (GUIControl* control: row) {
		control->SetVisible(fixed);
		control->SetEnabled(fixed);
	}
	// An undrawn row leaves no gap: the rows under it close up onto it.
	for (const auto& [control, rowY]: m_RowsUnderFixedDelay) {
		int x = 0, y = 0, width = 0, height = 0;
		control->GetControlRect(&x, &y, &width, &height);
		control->Move(x, fixed ? rowY : rowY - c_FixedDelayRowHeight);
	}
}

void SettingsNetworkGUI::SetActivePage(Page page) {
	// A page switch is a page leave for the page going away: its typed boxes commit first.
	if (page != m_ActivePage) {
		ApplyTextboxes();
	}
	m_ActivePage = page;
	for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
		m_PageBoxes[index]->SetVisible(index == static_cast<int>(page));
		m_PageTabs[index]->SetCheck(index == static_cast<int>(page));
	}
	UpdateStatusLines();
}

void SettingsNetworkGUI::UpdateStatusLines() {
}

void SettingsNetworkGUI::HandleInputEvents(GUIEvent& guiEvent) {
	if (guiEvent.GetType() != GUIEvent::Notification) {
		return;
	}
	if (guiEvent.GetMsg() == GUITab::UnPushed) {
		for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
			if (guiEvent.GetControl() == m_PageTabs[index]) {
				SetActivePage(static_cast<Page>(index));
				return;
			}
		}
	}
	if (guiEvent.GetControl() == m_DelayPolicyAutoRadio || guiEvent.GetControl() == m_DelayPolicyFixedRadio) {
		g_SettingsMan.SetNetworkHostDelayPolicy(m_DelayPolicyFixedRadio->GetCheck() ? SettingsMan::NetworkHostDelayPolicy::Fixed : SettingsMan::NetworkHostDelayPolicy::Auto);
		// Commit what is typed before the row holding it leaves the page.
		ApplyTextboxes();
	} else if (guiEvent.GetControl() == m_AutoRepairCheckbox) {
		g_SettingsMan.SetNetworkHostAutoRepair(m_AutoRepairCheckbox->GetCheck());
	} else if (guiEvent.GetControl() == m_ToastsCheckbox) {
		g_SettingsMan.SetNetworkToastsEnabled(m_ToastsCheckbox->GetCheck());
	} else if (guiEvent.GetControl() == m_PredictionCheckbox) {
		g_SettingsMan.SetLocalPredictionEnabled(m_PredictionCheckbox->GetCheck());
	} else if (guiEvent.GetControl() == m_StatusModeCombo && guiEvent.GetMsg() == GUIComboBox::Closed) {
		g_SettingsMan.SetNetworkMatchStatusMode(static_cast<SettingsMan::NetworkMatchStatusMode>(m_StatusModeCombo->GetSelectedIndex()));
	} else if ((guiEvent.GetControl() == m_DisplayNameTextbox || guiEvent.GetControl() == m_IdleWaitTextbox || guiEvent.GetControl() == m_FixedDelayTextbox) && guiEvent.GetMsg() == GUITextBox::Enter) {
		ApplyTextboxes();
		// Clicking off a focused text box must commit it too, otherwise it keeps the keyboard.
	} else if (guiEvent.GetMsg() == GUICollectionBox::Clicked &&
	           (guiEvent.GetControl() == m_NetworkSettingsBox ||
	            std::find(m_PageBoxes.begin(), m_PageBoxes.end(), guiEvent.GetControl()) != m_PageBoxes.end())) {
		auto* box = dynamic_cast<GUICollectionBox*>(guiEvent.GetControl());
		if (!box->HasFocus()) {
			ApplyTextboxes();
		}
	}
}
