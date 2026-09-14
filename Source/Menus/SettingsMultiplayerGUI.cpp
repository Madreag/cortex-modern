#include "SettingsMultiplayerGUI.h"
#include "SettingsMan.h"
#include "NetMatchService.h"
#include "NetReconnectUx.h"
#include "NetLockstep.h"
#include "TelemetryBundle.h"
#include "System.h"
#include "Writer.h"

#include "GUI.h"
#include "GUIButton.h"
#include "GUICheckbox.h"
#include "GUICollectionBox.h"
#include "GUIComboBox.h"
#include "GUILabel.h"
#include "GUITab.h"
#include "GUITextBox.h"

#include <SDL3/SDL.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace RTE;

namespace {

	std::string trimCopy(const std::string& value) {
		const auto first = value.find_first_not_of(" \t\n\r");
		if (first == std::string::npos) return "";
		const auto last = value.find_last_not_of(" \t\n\r");
		return value.substr(first, last - first + 1);
	}

	// Mirrors SettingsMan's SetNetworkDisplayName rules so an invalid entry stays on
	// the page with an explanation instead of being dropped silently by the setter.
	bool validDisplayName(const std::string& name) {
		if (name.empty() || name.size() > 64) return false;
		size_t characters = 0;
		for (size_t index = 0; index < name.size();) {
			const unsigned char lead = static_cast<unsigned char>(name[index]);
			size_t need = 0;
			if (lead < 0x80) {
				if (lead < 0x20 || lead == 0x7F) return false;
			} else if (lead < 0xC2) {
				return false;
			} else if (lead < 0xE0) {
				need = 1;
			} else if (lead < 0xF0) {
				need = 2;
			} else if (lead < 0xF5) {
				need = 3;
			} else {
				return false;
			}
			if (index + 1 + need > name.size()) return false;
			for (size_t trail = 1; trail <= need; ++trail) {
				if ((static_cast<unsigned char>(name[index + trail]) & 0xC0) != 0x80) return false;
			}
			if (need >= 1) {
				const unsigned char next = static_cast<unsigned char>(name[index + 1]);
				if (lead == 0xE0 && next < 0xA0) return false;
				if (lead == 0xED && next >= 0xA0) return false;
				if (lead == 0xF0 && next < 0x90) return false;
				if (lead == 0xF4 && next >= 0x90) return false;
			}
			if (++characters > 24) return false;
			index += 1 + need;
		}
		return true;
	}

	bool validDirectoryUrl(const std::string& url) {
		if (url.empty()) return true;
		const std::string scheme = url.rfind("https://", 0) == 0 ? "https://" : url.rfind("http://", 0) == 0 ? "http://" : "";
		return !scheme.empty() && url.size() > scheme.size();
	}

	// The codec compares the pin against a lowercase SHA-256 hex digest, so pasted
	// colons/spaces and casing are normalized before the value is stored.
	bool normalizeCertPin(std::string& pin) {
		std::string cleaned;
		for (unsigned char character: pin) {
			if (character == ':' || std::isspace(character)) continue;
			cleaned.push_back(static_cast<char>(std::tolower(character)));
		}
		pin = cleaned;
		if (pin.empty()) return true;
		if (pin.size() != 64) return false;
		for (unsigned char character: pin) {
			if (!std::isxdigit(character)) return false;
		}
		return true;
	}

	bool validDiagnosticsDirectory(const std::string& directory) {
		if (directory.empty()) return true;
		for (unsigned char character: directory) {
			if (character < 0x20 || character == 0x7F) return false;
		}
		const std::filesystem::path path(directory);
		if (!path.is_absolute()) return false;
		std::error_code error;
		std::filesystem::create_directories(path, error);
		if (error) return false;
		const std::filesystem::path probe = path / ".cc-write-check";
		{
			std::ofstream stream(probe);
			if (!stream.good()) return false;
		}
		std::filesystem::remove(probe, error);
		return true;
	}

	// TelemetryBundle writes under the working directory today; the persisted
	// diagnostics directory is not yet consumed by the bundle.
	std::string effectiveTelemetryDirectory() {
		return System::GetWorkingDirectory() + "Telemetry";
	}

	std::string autosavesDirectory() {
		return System::GetWorkingDirectory() + "Autosaves";
	}

	// Creates the directory when absent so the opened folder always exists, then
	// hands a file URI to the OS browser.
	bool openFolder(const std::string& directory) {
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		if (error) return false;
		std::string uri = "file:///";
		for (char character: std::filesystem::path(directory).generic_string()) {
			uri += character == ' ' ? "%20" : std::string(1, character);
		}
		return SDL_OpenURL(uri.c_str());
	}

	std::string latestFileName(const std::string& directory, const std::string& extension) {
		std::error_code error;
		std::string latest;
		std::filesystem::file_time_type latestTime{};
		for (const auto& entry: std::filesystem::directory_iterator(directory, error)) {
			if (!entry.is_regular_file() || entry.path().extension() != extension) continue;
			const auto written = entry.last_write_time(error);
			if (error) continue;
			if (latest.empty() || written > latestTime) {
				latestTime = written;
				latest = entry.path().filename().string();
			}
		}
		return latest;
	}

} // namespace

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

	m_DisplayNameBox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextMpDisplayName"));
	m_DisplayNameBox->SetMaxTextLength(64);
	m_NotificationsCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxMpNotifications"));
	m_PredictionCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxMpPrediction"));
	m_PlayerError = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpPlayerError"));

	m_ChatVisibleCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxMpChatVisible"));
	m_ChatSoundCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxMpChatSound"));
	m_ChatNotifyCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxMpChatNotify"));
	m_ChatScopeCombo = dynamic_cast<GUIComboBox*>(m_GUIControlManager->GetControl("ComboMpChatScope"));
	m_ChatScopeCombo->AddItem("All");
	m_ChatScopeCombo->AddItem("Team");
	m_ChatTextSizeCombo = dynamic_cast<GUIComboBox*>(m_GUIControlManager->GetControl("ComboMpChatTextSize"));
	m_ChatTextSizeCombo->AddItem("Small");
	m_ChatTextSizeCombo->AddItem("Large");
	m_MutedPlayersButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonMpMutedPlayers"));
	m_ChatError = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpChatError"));

	m_AutoReconnectCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxMpAutoReconnect"));
	m_OfferRejoinCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxMpOfferRejoin"));
	m_LastHostLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpLastHost"));
	m_RecoveryRecordLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpRecoveryRecord"));
	m_RecoveryStatusLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpRecoveryStatus"));
	m_RejoinButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonMpRejoin"));
	m_CancelRecoveryButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonMpCancelRecovery"));
	m_RecoveryError = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpRecoveryError"));

	m_AutosaveLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpAutosave"));
	m_AutosaveIntervalLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpAutosaveInterval"));
	m_AutosaveInfoLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpAutosaveInfo"));
	m_DiagDirBox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextMpDiagDir"));
	m_SaveDiagButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonMpSaveDiagnostics"));
	m_ReplaysButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonMpReplays"));
	m_RecordReplaysCheckbox = dynamic_cast<GUICheckbox*>(m_GUIControlManager->GetControl("CheckboxMpRecordReplays"));
	m_FilesMessage = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpFilesMessage"));

	m_DirUrlBox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextMpDirUrl"));
	m_DirPinBox = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("TextMpDirPin"));
	m_DirStatusLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpDirStatus"));
	m_InternetError = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("LabelMpInternetError"));

	ResetDraft();
	UpdateStatusLines();
	SetActivePage(Page::Player);
}

void SettingsMultiplayerGUI::SetEnabled(bool enable) {
	m_MultiplayerSettingsBox->SetVisible(enable);
	m_MultiplayerSettingsBox->SetEnabled(enable);
	if (enable) UpdateStatusLines();
}

void SettingsMultiplayerGUI::SetActivePage(Page page) {
	m_ActivePage = page;
	for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
		m_PageBoxes[index]->SetVisible(index == static_cast<int>(page));
		m_PageTabs[index]->SetCheck(index == static_cast<int>(page));
	}
}

void SettingsMultiplayerGUI::ResetDraft() {
	m_DisplayNameBox->SetText(g_SettingsMan.GetNetworkDisplayName());
	m_NotificationsCheckbox->SetCheck(g_SettingsMan.GetNetworkToastsEnabled());
	m_PredictionCheckbox->SetCheck(g_SettingsMan.GetLocalPrediction());

	m_ChatVisibleCheckbox->SetCheck(g_SettingsMan.GetNetworkChatVisible());
	m_ChatSoundCheckbox->SetCheck(g_SettingsMan.GetNetworkChatSound());
	m_ChatNotifyCheckbox->SetCheck(g_SettingsMan.GetNetworkChatNotify());
	m_ChatScopeCombo->SetSelectedIndex(static_cast<int>(g_SettingsMan.GetNetworkChatDefaultScope()));
	m_ChatTextSizeCombo->SetSelectedIndex(static_cast<int>(g_SettingsMan.GetNetworkChatTextSize()));

	m_AutoReconnectCheckbox->SetCheck(g_SettingsMan.GetNetworkAutoReconnect());
	m_OfferRejoinCheckbox->SetCheck(g_SettingsMan.GetNetworkOfferStoredRejoin());

	m_DiagDirBox->SetText(g_SettingsMan.GetNetworkDiagnosticsDirectory());
	m_RecordReplaysCheckbox->SetCheck(g_SettingsMan.GetNetworkRecordReplays());

	m_DirUrlBox->SetText(g_SettingsMan.GetSessionDirectoryUrl());
	m_DirPinBox->SetText(g_SettingsMan.GetSessionDirectoryCertSha256());
}

void SettingsMultiplayerGUI::ResetPageDefaults(Page page) {
	switch (page) {
		case Page::Player:
			m_DisplayNameBox->SetText("Player");
			m_NotificationsCheckbox->SetCheck(true);
			m_PredictionCheckbox->SetCheck(true);
			break;
		case Page::Chat:
			m_ChatVisibleCheckbox->SetCheck(true);
			m_ChatSoundCheckbox->SetCheck(false);
			m_ChatNotifyCheckbox->SetCheck(true);
			m_ChatScopeCombo->SetSelectedIndex(0);
			m_ChatTextSizeCombo->SetSelectedIndex(0);
			break;
		case Page::Internet:
			m_DirUrlBox->SetText("");
			m_DirPinBox->SetText("");
			break;
		default:
			break;
	}
	UpdateStatusLines();
}

bool SettingsMultiplayerGUI::DraftMatchesSettings() const {
	return m_DisplayNameBox->GetText() == g_SettingsMan.GetNetworkDisplayName() &&
		(m_NotificationsCheckbox->GetCheck() != 0) == g_SettingsMan.GetNetworkToastsEnabled() &&
		(m_PredictionCheckbox->GetCheck() != 0) == g_SettingsMan.GetLocalPrediction() &&
		(m_ChatVisibleCheckbox->GetCheck() != 0) == g_SettingsMan.GetNetworkChatVisible() &&
		(m_ChatSoundCheckbox->GetCheck() != 0) == g_SettingsMan.GetNetworkChatSound() &&
		(m_ChatNotifyCheckbox->GetCheck() != 0) == g_SettingsMan.GetNetworkChatNotify() &&
		m_ChatScopeCombo->GetSelectedIndex() == static_cast<int>(g_SettingsMan.GetNetworkChatDefaultScope()) &&
		m_ChatTextSizeCombo->GetSelectedIndex() == static_cast<int>(g_SettingsMan.GetNetworkChatTextSize()) &&
		(m_AutoReconnectCheckbox->GetCheck() != 0) == g_SettingsMan.GetNetworkAutoReconnect() &&
		(m_OfferRejoinCheckbox->GetCheck() != 0) == g_SettingsMan.GetNetworkOfferStoredRejoin() &&
		m_DiagDirBox->GetText() == g_SettingsMan.GetNetworkDiagnosticsDirectory() &&
		(m_RecordReplaysCheckbox->GetCheck() != 0) == g_SettingsMan.GetNetworkRecordReplays() &&
		m_DirUrlBox->GetText() == g_SettingsMan.GetSessionDirectoryUrl() &&
		m_DirPinBox->GetText() == g_SettingsMan.GetSessionDirectoryCertSha256();
}

void SettingsMultiplayerGUI::FailOnPage(Page page, const std::string& text, GUIControl* focus) {
	SetActivePage(page);
	GUILabel* error = m_PlayerError;
	if (page == Page::Chat) error = m_ChatError;
	else if (page == Page::Recovery) error = m_RecoveryError;
	else if (page == Page::Files) error = m_FilesMessage;
	else if (page == Page::Internet) error = m_InternetError;
	error->SetText(text);
	if (focus && focus->GetPanel()) m_GUIControlManager->GetManager()->SetFocus(focus->GetPanel());
}

void SettingsMultiplayerGUI::ApplyDraft() {
	const std::string name = trimCopy(m_DisplayNameBox->GetText());
	if (!validDisplayName(name)) {
		FailOnPage(Page::Player, "Display name must be 1-24 printable characters.", m_DisplayNameBox);
		return;
	}
	std::string pin = trimCopy(m_DirPinBox->GetText());
	if (!normalizeCertPin(pin)) {
		FailOnPage(Page::Internet, "Certificate pin must be the 64 hex digits of the SHA-256 fingerprint.", m_DirPinBox);
		return;
	}
	const std::string url = trimCopy(m_DirUrlBox->GetText());
	if (!validDirectoryUrl(url)) {
		FailOnPage(Page::Internet, "Session directory must be an http(s):// URL.", m_DirUrlBox);
		return;
	}
	const std::string directory = trimCopy(m_DiagDirBox->GetText());
	if (!validDiagnosticsDirectory(directory)) {
		FailOnPage(Page::Files, "Diagnostics folder must be an absolute, writable path.", m_DiagDirBox);
		return;
	}

	g_SettingsMan.SetNetworkDisplayName(name);
	g_SettingsMan.SetNetworkToastsEnabled(m_NotificationsCheckbox->GetCheck() != 0);
	g_SettingsMan.SetLocalPrediction(m_PredictionCheckbox->GetCheck() != 0);
	g_SettingsMan.SetNetworkChatVisible(m_ChatVisibleCheckbox->GetCheck() != 0);
	g_SettingsMan.SetNetworkChatSound(m_ChatSoundCheckbox->GetCheck() != 0);
	g_SettingsMan.SetNetworkChatNotify(m_ChatNotifyCheckbox->GetCheck() != 0);
	g_SettingsMan.SetNetworkChatDefaultScope(static_cast<SettingsMan::NetworkChatDefaultScope>(m_ChatScopeCombo->GetSelectedIndex()));
	g_SettingsMan.SetNetworkChatTextSize(static_cast<SettingsMan::NetworkChatTextSize>(m_ChatTextSizeCombo->GetSelectedIndex()));
	g_SettingsMan.SetNetworkAutoReconnect(m_AutoReconnectCheckbox->GetCheck() != 0);
	g_SettingsMan.SetNetworkOfferStoredRejoin(m_OfferRejoinCheckbox->GetCheck() != 0);
	g_SettingsMan.SetNetworkDiagnosticsDirectory(directory);
	g_SettingsMan.SetNetworkRecordReplays(m_RecordReplaysCheckbox->GetCheck() != 0);
	g_SettingsMan.SetSessionDirectoryUrl(url);
	g_SettingsMan.SetSessionDirectoryCertSha256(pin);

	Writer writer(System::GetUserdataDirectory() + "Settings.ini");
	g_SettingsMan.Save(writer);
	m_PendingSave = !writer.WriterOK();

	ResetDraft();
	UpdateStatusLines();
	if (m_PendingSave) {
		FailOnPage(m_ActivePage, "Applied for this run; could not save settings — press Apply to retry.", nullptr);
	}
}

void SettingsMultiplayerGUI::UpdateDiagnosticsStatus() {
	if (TelemetryBundle::IsBusy()) {
		m_FilesMessage->SetText("Saving diagnostics…");
		m_DiagWasBusy = true;
		return;
	}
	const std::string latest = latestFileName(effectiveTelemetryDirectory(), ".zip");
	if (m_DiagWasBusy) {
		m_FilesMessage->SetText(latest.empty() ? "Diagnostics saved." : "Diagnostics saved: " + latest);
		m_DiagWasBusy = false;
	} else {
		m_FilesMessage->SetText(latest.empty() ? "No diagnostics saved yet." : "Latest: " + latest);
	}
}

void SettingsMultiplayerGUI::UpdateStatusLines() {
	NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
	m_LastHostLabel->SetText(reconnect.GetOfferAddress().empty() ? "—" : reconnect.GetOfferAddress());
	const char* record = "—";
	switch (reconnect.GetOffer()) {
		case NetReconnectOffer::Available: record = "Rejoin available"; break;
		case NetReconnectOffer::Corrupt: record = "Unreadable"; break;
		case NetReconnectOffer::Stale: record = "Expired"; break;
		case NetReconnectOffer::Missing: record = "No recovery record"; break;
		default: break;
	}
	m_RecoveryRecordLabel->SetText(record);
	const std::string status = reconnect.GetStatusText();
	m_RecoveryStatusLabel->SetText(status.empty() ? "—" : status);
	m_RejoinButton->SetEnabled(reconnect.GetOffer() == NetReconnectOffer::Available || reconnect.CanRetryManually());
	m_CancelRecoveryButton->SetEnabled(reconnect.CanCancel());

	const std::string latestSave = latestFileName(autosavesDirectory(), ".ccsave");
	m_AutosaveInfoLabel->SetText("Keep 3 / Latest: " + (latestSave.empty() ? "—" : latestSave));

	UpdateDiagnosticsStatus();
	m_DirStatusLabel->SetText(g_SettingsMan.GetSessionDirectoryUrl().empty() ? "Not configured" : "Configured");
	m_SaveDiagButton->SetEnabled(!TelemetryBundle::IsBusy());
	m_ApplyButton->SetEnabled(m_PendingSave || !DraftMatchesSettings());
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
	if (guiEvent.GetType() == GUIEvent::Command) {
		const std::string control = guiEvent.GetControl()->GetName();
		if (guiEvent.GetControl() == m_ApplyButton) {
			ApplyDraft();
		} else if (control == "ButtonMpPlayerDefaults") {
			ResetPageDefaults(Page::Player);
		} else if (control == "ButtonMpChatDefaults") {
			ResetPageDefaults(Page::Chat);
		} else if (control == "ButtonMpInternetDefaults") {
			ResetPageDefaults(Page::Internet);
		} else if (guiEvent.GetControl() == m_SaveDiagButton) {
			if (!TelemetryBundle::RequestCapture()) {
				FailOnPage(Page::Files, "Diagnostics are already being saved.", nullptr);
			}
		} else if (control == "ButtonMpOpenAutosaves") {
			if (!openFolder(autosavesDirectory())) {
				FailOnPage(Page::Files, "Could not open the autosaves folder.", nullptr);
			}
		} else if (control == "ButtonMpOpenDiagDir") {
			if (!openFolder(effectiveTelemetryDirectory())) {
				FailOnPage(Page::Files, "Could not open the diagnostics folder.", nullptr);
			}
		} else if (control == "ButtonMpCopyDiagPath") {
			if (!SDL_SetClipboardText(effectiveTelemetryDirectory().c_str())) {
				FailOnPage(Page::Files, "Could not copy the folder path.", nullptr);
			}
		} else if (guiEvent.GetControl() == m_RejoinButton) {
			NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
			reconnect.RequestManualRetry(NetLockstepNowMs());
			reconnect.DismissOffer();
			std::string rejoinError;
			if (!g_NetMatchService.BeginTicketRejoin(&rejoinError)) {
				reconnect.NoteAttemptFailed(NetLockstepNowMs(), rejoinError);
				FailOnPage(Page::Recovery, rejoinError.empty() ? "Rejoin failed." : rejoinError, nullptr);
			} else {
				reconnect.NoteAttemptStarted(NetLockstepNowMs());
			}
		} else if (guiEvent.GetControl() == m_CancelRecoveryButton) {
			NetReconnectUx& reconnect = g_NetMatchService.GetReconnectUx();
			reconnect.Cancel(NetLockstepNowMs());
			reconnect.DismissOffer();
		}
	}
	UpdateStatusLines();
}
