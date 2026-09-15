#include "SettingsNetworkGUI.h"
#include "SettingsMan.h"
#include "NetMatchConfig.h"
#include "NetMatchService.h"
#include "NetReconnectUx.h"
#include "NetLockstep.h"

#include "GUI.h"
#include "GUIButton.h"
#include "GUICollectionBox.h"
#include "GUICheckbox.h"
#include "GUIComboBox.h"
#include "GUILabel.h"
#include "GUIRadioButton.h"
#include "GUITab.h"
#include "GUITextBox.h"

#include <algorithm>
#include <charconv>
#include <string>

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

	m_ChatVisibleCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkChatVisible"));
	m_ChatSoundCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkChatSound"));
	m_ChatNotifyCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkChatNotify"));
	m_ChatScopeCombo = dynamic_cast<GUIComboBox*>(m_GUIControlManager->GetControl("ComboNetworkChatScope"));
	m_ChatScopeCombo->AddItem("All");
	m_ChatScopeCombo->AddItem("Team");
	m_ChatTextSizeCombo = dynamic_cast<GUIComboBox*>(m_GUIControlManager->GetControl("ComboNetworkChatTextSize"));
	m_ChatTextSizeCombo->AddItem("Small");
	m_ChatTextSizeCombo->AddItem("Large");

	m_AutoReconnectCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkAutoReconnect"));
	m_OfferRejoinCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkOfferRejoin"));
	m_LastHostLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetLastHost"));
	m_RecoveryRecordLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetRecoveryRecord"));
	m_RecoveryStatusLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetRecoveryStatus"));
	m_RecoveryError = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetRecoveryError"));
	m_RejoinButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonNetRejoin"));
	m_CancelRecoveryButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonNetCancelRecovery"));

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
	m_ChatVisibleCheckbox->SetCheck(g_SettingsMan.GetNetworkChatVisible());
	m_ChatSoundCheckbox->SetCheck(g_SettingsMan.GetNetworkChatSound());
	m_ChatNotifyCheckbox->SetCheck(g_SettingsMan.GetNetworkChatNotify());
	m_ChatScopeCombo->SetSelectedIndex(static_cast<int>(g_SettingsMan.GetNetworkChatDefaultScope()));
	m_ChatTextSizeCombo->SetSelectedIndex(static_cast<int>(g_SettingsMan.GetNetworkChatTextSize()));
	m_AutoReconnectCheckbox->SetCheck(g_SettingsMan.GetNetworkAutoReconnect());
	m_OfferRejoinCheckbox->SetCheck(g_SettingsMan.GetNetworkOfferStoredRejoin());
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
	m_RecoveryError->SetText("");
	UpdateStatusLines();
}

void SettingsNetworkGUI::UpdateStatusLines() {
	NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
	m_LastHostLabel->SetText(reconnect.GetOfferAddress().empty() ? "-" : reconnect.GetOfferAddress());
	const char* record = "-";
	switch (reconnect.GetOffer()) {
		case NetReconnectOffer::Available: record = "Rejoin available"; break;
		case NetReconnectOffer::Corrupt: record = "Unreadable"; break;
		case NetReconnectOffer::Stale: record = "Expired"; break;
		case NetReconnectOffer::Missing: record = "No recovery record"; break;
		default: break;
	}
	m_RecoveryRecordLabel->SetText(record);
	const std::string status = reconnect.GetStatusText();
	m_RecoveryStatusLabel->SetText(status.empty() ? "-" : status);
	m_RejoinButton->SetEnabled(reconnect.GetOffer() == NetReconnectOffer::Available || reconnect.CanRetryManually());
	m_CancelRecoveryButton->SetEnabled(reconnect.CanCancel());
}

void SettingsNetworkGUI::HandleInputEvents(GUIEvent& guiEvent) {
	if (guiEvent.GetType() == GUIEvent::Command) {
		if (guiEvent.GetControl() == m_RejoinButton) {
			NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
			reconnect.RequestManualRetry(NetLockstepNowMs());
			reconnect.DismissOffer();
			std::string rejoinError;
			if (!g_NetMatchService.BeginTicketRejoin(&rejoinError)) {
				reconnect.NoteAttemptFailed(NetLockstepNowMs(), rejoinError);
				m_RecoveryError->SetText(rejoinError.empty() ? "Rejoin failed." : rejoinError);
			} else {
				reconnect.NoteAttemptStarted(NetLockstepNowMs());
			}
		} else if (guiEvent.GetControl() == m_CancelRecoveryButton) {
			NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
			reconnect.Cancel(NetLockstepNowMs());
			reconnect.DismissOffer();
		} else {
			return;
		}
		UpdateStatusLines();
		return;
	}
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
	} else if (guiEvent.GetControl() == m_ChatVisibleCheckbox) {
		g_SettingsMan.SetNetworkChatVisible(m_ChatVisibleCheckbox->GetCheck());
	} else if (guiEvent.GetControl() == m_ChatSoundCheckbox) {
		g_SettingsMan.SetNetworkChatSound(m_ChatSoundCheckbox->GetCheck());
	} else if (guiEvent.GetControl() == m_ChatNotifyCheckbox) {
		g_SettingsMan.SetNetworkChatNotify(m_ChatNotifyCheckbox->GetCheck());
	} else if (guiEvent.GetControl() == m_ChatScopeCombo && guiEvent.GetMsg() == GUIComboBox::Closed) {
		g_SettingsMan.SetNetworkChatDefaultScope(static_cast<SettingsMan::NetworkChatDefaultScope>(m_ChatScopeCombo->GetSelectedIndex()));
	} else if (guiEvent.GetControl() == m_ChatTextSizeCombo && guiEvent.GetMsg() == GUIComboBox::Closed) {
		g_SettingsMan.SetNetworkChatTextSize(static_cast<SettingsMan::NetworkChatTextSize>(m_ChatTextSizeCombo->GetSelectedIndex()));
	} else if (guiEvent.GetControl() == m_AutoReconnectCheckbox) {
		g_SettingsMan.SetNetworkAutoReconnect(m_AutoReconnectCheckbox->GetCheck());
	} else if (guiEvent.GetControl() == m_OfferRejoinCheckbox) {
		g_SettingsMan.SetNetworkOfferStoredRejoin(m_OfferRejoinCheckbox->GetCheck());
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
