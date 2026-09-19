#include "SettingsNetworkGUI.h"
#include "SettingsMan.h"
#include "NetMatchConfig.h"
#include "NetMatchService.h"
#include "NetReconnectUx.h"
#include "NetLockstep.h"
#include "TelemetryBundle.h"
#include "System.h"
#include "GUIUtil.h"

#include "GUI.h"
#include "GUIButton.h"
#include "GUICollectionBox.h"
#include "GUICheckbox.h"
#include "GUIComboBox.h"
#include "GUILabel.h"
#include "GUIRadioButton.h"
#include "GUITab.h"
#include "GUITextBox.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
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

	// TelemetryBundle writes under the working directory today; the persisted
	// diagnostics directory is not yet consumed by the bundle.
	std::string EffectiveTelemetryDirectory() {
		return System::GetWorkingDirectory() + "Telemetry";
	}

	std::string AutosavesDirectory() {
		return AutosaveStore::Directory().string();
	}

	// Creates the directory when absent so the opened folder always exists, then
	// hands a file URI to the OS browser.
	bool OpenFolder(const std::string& directory) {
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		if (error) {
			return false;
		}
		std::string uri = "file:///";
		for (char character: std::filesystem::path(directory).generic_string()) {
			uri += character == ' ' ? "%20" : std::string(1, character);
		}
		return SDL_OpenURL(uri.c_str());
	}

	std::string LatestFileName(const std::string& directory, const std::string& extension) {
		std::error_code error;
		std::string latest;
		std::filesystem::file_time_type latestTime{};
		for (const auto& entry: std::filesystem::directory_iterator(directory, error)) {
			if (!entry.is_regular_file() || entry.path().extension() != extension) {
				continue;
			}
			const auto written = entry.last_write_time(error);
			if (error) {
				continue;
			}
			if (latest.empty() || written > latestTime) {
				latestTime = written;
				latest = entry.path().filename().string();
			}
		}
		return latest;
	}

	// The ini reader cuts values at "//" (a comment), so the persisted form is the
	// scheme-less host[:port][/path] the directory client puts https:// back in front of.
	// A pasted https:// prefix is normalized away before this sees the value; the rest
	// of what cannot persist (an http:// scheme, any remaining "//", whitespace) refuses.
	bool ValidDirectoryUrl(const std::string& url) {
		if (url.empty()) {
			return true;
		}
		if (url.compare(0, 7, "http://") == 0 || url.find("//") != std::string::npos) {
			return false;
		}
		return url.find_first_of(" \t") == std::string::npos;
	}

	// A pin is a SHA-256 hex digest; pasted values may carry colons or spaces, which are
	// not part of the pin. Anything left that is not exactly 64 hex characters is refused.
	bool NormalizeCertPin(const std::string& raw, std::string& normalized) {
		normalized.clear();
		for (unsigned char character: raw) {
			if (character == ':' || std::isspace(character)) {
				continue;
			}
			normalized += static_cast<char>(std::tolower(character));
		}
		if (normalized.empty()) {
			return true;
		}
		if (normalized.size() != 64) {
			return false;
		}
		for (char character: normalized) {
			if (!std::isxdigit(static_cast<unsigned char>(character))) {
				return false;
			}
		}
		return true;
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

	m_AutosaveLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetAutosave"));
	m_AutosaveIntervalLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetAutosaveInterval"));
	m_AutosaveInfoLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetAutosaveInfo"));
	m_DiagDirTextbox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextNetworkDiagDir"));
	m_SaveDiagButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonNetSaveDiagnostics"));
	m_RecordReplaysCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxNetworkRecordReplays"));
	m_FilesMessage = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetFilesMessage"));

	m_DirUrlTextbox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextNetworkDirUrl"));
	m_DirUrlHintLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetDirUrlHint"));
	// The ini reader cuts values at "//", so the hint's https:// has to come from here.
	m_DirUrlHintLabel->SetText("host[:port][/path] - https:// is implied");
	m_DirPinTextbox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextNetworkDirPin"));
	m_DirStatusLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetDirStatus"));
	m_InternetError = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelNetInternetError"));

	const auto rowTop = [](GUIControl* control) {
		int x = 0, y = 0, width = 0, height = 0;
		control->GetControlRect(&x, &y, &width, &height);
		return std::make_pair(control, y);
	};
	m_RowsUnderFixedDelay = {rowTop(m_IdleWaitLabel), rowTop(m_IdleWaitTextbox), rowTop(m_IdleWaitHintLabel), rowTop(m_AutoRepairCheckbox),
	                         rowTop(m_ToastsCheckbox), rowTop(m_PredictionCheckbox), rowTop(m_GUIControlManager->GetControl("LabelMatchStatusWidget")), rowTop(m_StatusModeCombo)};

	ShowSavedValues();
	// The skin draws only the player page's box first; checking its tab keeps the selector in step.
	SetActivePage(Page::Player);
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
	m_DiagDirTextbox->SetText(g_SettingsMan.GetNetworkDiagnosticsDirectory());
	m_RecordReplaysCheckbox->SetCheck(g_SettingsMan.GetNetworkRecordReplays());
	m_DirUrlTextbox->SetText(g_SettingsMan.GetSessionDirectoryUrl());
	m_DirPinTextbox->SetText(g_SettingsMan.GetSessionDirectoryCertSha256());
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
	const std::string diagDir = m_DiagDirTextbox->GetText();
	g_SettingsMan.SetNetworkDiagnosticsDirectory(diagDir);
	const bool diagDirRefused = g_SettingsMan.GetNetworkDiagnosticsDirectory() != diagDir;

	// Each box commits only when its own value passes; a refused value keeps the stored
	// setting and the box reverts to it through ShowSavedValues.
	std::string internetError;
	std::string url = m_DirUrlTextbox->GetText();
	if (url.compare(0, 8, "https://") == 0) {
		url.erase(0, 8);
	}
	if (ValidDirectoryUrl(url)) {
		g_SettingsMan.SetSessionDirectoryUrl(url);
	} else {
		internetError = "Enter the directory as host[:port][/path] - https:// is implied.";
	}
	std::string pin;
	if (NormalizeCertPin(m_DirPinTextbox->GetText(), pin)) {
		g_SettingsMan.SetSessionDirectoryCertSha256(pin);
	} else {
		internetError = "Certificate pin must be 64 hexadecimal characters.";
	}

	// A refused value never stays on screen: the settings are what the page states.
	ShowSavedValues();
	if (diagDirRefused) {
		m_FilesMessage->SetText("Diagnostics folder refused: no control characters allowed.");
	}
	m_InternetError->SetText(internetError);
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
	m_InternetError->SetText("");
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

	const uint32_t autosaveSeconds = g_SettingsMan.GetAutosaveSeconds();
	m_AutosaveLabel->SetText(autosaveSeconds > 0 ? "Enabled" : "Disabled");
	m_AutosaveIntervalLabel->SetText(autosaveSeconds > 0 ? std::to_string(autosaveSeconds) + " s" : "-");
	const std::string latestSave = LatestFileName(AutosavesDirectory(), ".ccsave");
	m_AutosaveInfoLabel->SetText("Latest: " + (latestSave.empty() ? std::string("-") : latestSave));

	m_SaveDiagButton->SetEnabled(!TelemetryBundle::IsBusy());
	if (TelemetryBundle::IsBusy()) {
		m_FilesMessage->SetText("Saving diagnostics...");
	} else {
		const std::string latest = LatestFileName(EffectiveTelemetryDirectory(), ".zip");
		m_FilesMessage->SetText(latest.empty() ? "No diagnostics saved yet." : "Latest: " + latest);
	}

	m_DirStatusLabel->SetText(g_SettingsMan.GetSessionDirectoryUrl().empty() ? "Not configured" : "Configured");
}

void SettingsNetworkGUI::HandleInputEvents(GUIEvent& guiEvent) {
	if (guiEvent.GetType() == GUIEvent::Command) {
		// Action feedback goes on the page's message line AFTER the status refresh,
		// which owns the line's resting text.
		std::string message;
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
		} else if (guiEvent.GetControl() == m_SaveDiagButton) {
			if (!TelemetryBundle::RequestCapture()) {
				message = "Diagnostics are already being saved.";
			}
		} else if (guiEvent.GetControl()->GetName() == "ButtonNetOpenAutosaves") {
			if (!OpenFolder(AutosavesDirectory())) {
				message = "Could not open the autosaves folder.";
			}
		} else if (guiEvent.GetControl()->GetName() == "ButtonNetCopyAutosavesPath") {
			if (!GUIUtil::SetClipboardText(AutosavesDirectory())) {
				message = "Could not copy the folder path.";
			} else {
				message = "Copied " + AutosavesDirectory();
			}
		} else if (guiEvent.GetControl()->GetName() == "ButtonNetOpenDiagnostics") {
			if (!OpenFolder(EffectiveTelemetryDirectory())) {
				message = "Could not open the diagnostics folder.";
			}
		} else if (guiEvent.GetControl()->GetName() == "ButtonNetCopyDiagPath") {
			if (!GUIUtil::SetClipboardText(EffectiveTelemetryDirectory())) {
				message = "Could not copy the folder path.";
			} else {
				message = "Copied " + EffectiveTelemetryDirectory();
			}
		} else {
			return;
		}
		UpdateStatusLines();
		if (!message.empty()) {
			m_FilesMessage->SetText(message);
		}
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
	} else if (guiEvent.GetControl() == m_RecordReplaysCheckbox) {
		g_SettingsMan.SetNetworkRecordReplays(m_RecordReplaysCheckbox->GetCheck());
	} else if ((guiEvent.GetControl() == m_DisplayNameTextbox || guiEvent.GetControl() == m_IdleWaitTextbox || guiEvent.GetControl() == m_FixedDelayTextbox || guiEvent.GetControl() == m_DiagDirTextbox || guiEvent.GetControl() == m_DirUrlTextbox || guiEvent.GetControl() == m_DirPinTextbox) && guiEvent.GetMsg() == GUITextBox::Enter) {
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
