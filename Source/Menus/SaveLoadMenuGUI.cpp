#include "SaveLoadMenuGUI.h"

#include "ActivityMan.h"
#include "PresetMan.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "SaveGameArchive.h"

#include "PauseMenuGUI.h"
#include "SettingsGUI.h"
#include "ModManagerGUI.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "GAScripted.h"
#include "GUIInputWrapper.h"
#include "GUICollectionBox.h"
#include "GUILabel.h"
#include "GUIButton.h"
#include "GUIListBox.h"
#include "GUITextBox.h"
#include "GUIComboBox.h"

#include <execution>
#include <iostream>

using namespace RTE;

SaveLoadMenuGUI::SaveLoadMenuGUI(AllegroScreen* guiScreen, GUIInputWrapper* guiInput, bool createForPauseMenu) {
	m_GUIControlManager = std::make_unique<GUIControlManager>();
	RTEAssert(m_GUIControlManager->Create(guiScreen, guiInput, "Base.rte/GUIs/Skins/Menus", "MainMenuSubMenuSkin.ini"), "Failed to create GUI Control Manager and load it from Base.rte/GUIs/Skins/Menus/MainMenuSubMenuSkin.ini");
	m_GUIControlManager->Load("Base.rte/GUIs/SaveLoadMenuGUI.ini");

	int rootBoxMaxWidth = g_WindowMan.FullyCoversAllDisplays() ? g_WindowMan.GetPrimaryWindowDisplayWidth() / g_WindowMan.GetResMultiplier() : g_WindowMan.GetResX();

	GUICollectionBox* rootBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("root"));
	rootBox->Resize(rootBoxMaxWidth, g_WindowMan.GetResY());

	m_SaveGameMenuBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("CollectionBoxSaveGameMenu"));
	m_SaveGameMenuBox->CenterInParent(true, true);
	m_SaveGameMenuBox->SetPositionAbs(m_SaveGameMenuBox->GetXPos(), (rootBox->GetHeight() < 540) ? m_SaveGameMenuBox->GetYPos() - 15 : 140);

	m_OrderByComboBox = dynamic_cast<GUIComboBox*>(m_GUIControlManager->GetControl("ComboOrderBy"));
	m_OrderByComboBox->AddItem("Name");
	m_OrderByComboBox->AddItem("Date");
	m_OrderByComboBox->AddItem("Activity");
	m_OrderByComboBox->SetSelectedIndex(1); // order by Date by default

	m_BackToMainButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonBackToMainMenu"));

	if (createForPauseMenu) {
		m_BackToMainButton->SetSize(120, 20);
		m_BackToMainButton->SetText("Back to Pause Menu");
	}
	m_BackToMainButton->SetPositionAbs((rootBox->GetWidth() - m_BackToMainButton->GetWidth()) / 2, m_SaveGameMenuBox->GetYPos() + m_SaveGameMenuBox->GetHeight() + 10);

	m_SaveGamesListBox = dynamic_cast<GUIListBox*>(m_GUIControlManager->GetControl("ListBoxSaveGames"));
	m_SaveGamesListBox->SetFont(m_GUIControlManager->GetSkin()->GetFont("FontConsoleMonospace.png"));
	m_SaveGamesListBox->SetMouseScrolling(true);
	m_SaveGamesListBox->SetScrollBarThickness(15);
	m_SaveGamesListBox->SetScrollBarPadding(2);
	m_SaveGamesListBox->SetHighlightAsIfAlwaysFocused(true);

	m_SaveGameName = dynamic_cast<GUITextBox*>(m_GUIControlManager->GetControl("SaveGameName"));
	m_LoadButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonLoad"));
	m_CreateButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonCreate"));
	m_OverwriteButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonOverwrite"));
	m_DeleteButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ButtonDelete"));
	m_DescriptionLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("DescriptionLabel"));

	m_ConfirmationBox = dynamic_cast<GUICollectionBox*>(m_GUIControlManager->GetControl("ConfirmDialog"));
	m_ConfirmationBox->CenterInParent(true, true);

	m_ConfirmationLabel = dynamic_cast<GUILabel*>(m_GUIControlManager->GetControl("ConfirmLabel"));
	m_ConfirmationButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("ConfirmButton"));
	m_CancelButton = dynamic_cast<GUIButton*>(m_GUIControlManager->GetControl("CancelButton"));

	m_SaveGamesFetched = false;
	m_WasSaving = false;
	m_SavingBlinkTimer.SetRealTimeLimitS(1.5f);

	SwitchToConfirmDialogMode(ConfirmDialogMode::None);
}

void SaveLoadMenuGUI::PopulateSaveGamesList() {
	if (g_ActivityMan.IsCurrentlySaving() || m_SaveGamesFetched) {
		return;
	}

	m_SaveGames.clear();

	m_GUIControlManager->GetManager()->SetFocus(nullptr);

	const std::string saveFilePath = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/";
	std::error_code directoryError;
	for (std::filesystem::directory_iterator entry(saveFilePath, directoryError), end; !directoryError && entry != end; entry.increment(directoryError)) {
		if (entry->path().extension() == ".ccsave" && entry->is_regular_file(directoryError)) {
			SaveRecord record;
			record.SavePath = entry->path();
			record.SaveDate = entry->last_write_time(directoryError);
			if (!directoryError) m_SaveGames.push_back(std::move(record));
		}
	}

	std::for_each(std::execution::par, m_SaveGames.begin(), m_SaveGames.end(), [](SaveRecord& record) {
		try {
			SaveGameArchive archive(record.SavePath.string());
			std::string text;
			archive.ReadEntry("Index.ini", text);
			if (text.empty() || text.find('\0') != std::string::npos) throw std::runtime_error("invalid index");
			Reader reader(std::make_unique<std::istringstream>(text), record.SavePath.string(), true, nullptr, true);
			reader.SetThrowOnError(true);
			reader.SetSkipIncludes(true);
			std::string activity, scene;
			while (reader.NextProperty()) {
				const std::string propName = reader.ReadPropName();
				if (propName == "ActivityName") reader >> activity;
				else if (propName == "OriginalScenePresetName") reader >> scene;
				else reader.ReadPropValue();
			}
			if (activity.empty() || scene.empty()) throw std::runtime_error("incomplete index");
			record.Activity = std::move(activity);
			record.Scene = std::move(scene);
		} catch (const std::exception&) {
			record.Activity = "Save details unavailable";
			record.Scene.clear();
		}
	});

	UpdateSaveGamesGUIList();
	m_SaveGamesFetched = true;
}

void SaveLoadMenuGUI::UpdateSaveGamesGUIList() {
	const std::string& currentOrder = m_OrderByComboBox->GetSelectedItem()->m_Name;
	if (currentOrder == "Name") {
		std::stable_sort(m_SaveGames.begin(), m_SaveGames.end(), [](const SaveRecord& lhs, const SaveRecord& rhs) { return lhs.SavePath.stem().string() < rhs.SavePath.stem().string(); });
	} else if (currentOrder == "Date") {
		std::stable_sort(m_SaveGames.begin(), m_SaveGames.end(), [](const SaveRecord& lhs, const SaveRecord& rhs) { return lhs.SaveDate > rhs.SaveDate; });
	} else if (currentOrder == "Activity") {
		std::stable_sort(m_SaveGames.begin(), m_SaveGames.end(), [](const SaveRecord& lhs, const SaveRecord& rhs) { return lhs.Activity < rhs.Activity; });
	}

	m_SaveGamesListBox->ClearList();
	for (int i = 0; i < m_SaveGames.size(); i++) {
		const SaveRecord& save = m_SaveGames[i];

		std::stringstream saveNameText;
		saveNameText << std::left << std::setfill(' ') << std::setw(32) << save.SavePath.stem().string();

		// This is so much more fucking difficult than it has any right to be
#if defined(_MSC_VER) || __GNUC__ > 12
		const auto saveFsTime = std::chrono::clock_cast<std::chrono::system_clock>(save.SaveDate);
		const auto saveTime = std::chrono::system_clock::to_time_t(saveFsTime);
#else
		// TODO - kill this monstrosity when we move to GCC13
		// libc++ file_clock rep is wider than system_clock; duration_cast lands it in range first.
#if defined(_LIBCPP_VERSION)
		auto saveFsTime = std::chrono::system_clock::time_point(
		    std::chrono::duration_cast<std::chrono::system_clock::duration>(save.SaveDate.time_since_epoch()));
#else
		auto saveFsTime = std::chrono::system_clock::time_point(save.SaveDate.time_since_epoch());
#endif
#ifdef _WIN32
		// Windows epoch time are the number of seconds since... 1601-01-01 00:00:00. Seriously.
		saveFsTime -= std::chrono::seconds(11644473600LL);
#elif defined(__GLIBCXX__)
		// libstdc++ file_clock epoch is 2174-01-01 00:00:00 for reasons
		saveFsTime += std::chrono::seconds(6437664000LL);
#endif
		const auto saveTime = std::chrono::system_clock::to_time_t(saveFsTime);
#endif
		const auto saveTimeLocal = std::localtime(&saveTime);

		std::stringstream saveDateTimeText;
		saveDateTimeText << std::put_time(saveTimeLocal, "%Y-%m-%d %X");

		m_SaveGamesListBox->AddItem(" " + saveNameText.str() + "" + save.Scene + " - " + save.Activity + "", saveDateTimeText.str() + " ", nullptr, nullptr, i);
	}

	m_SaveGamesListBox->ScrollToTop();
}

bool SaveLoadMenuGUI::LoadSave() {
	bool success = g_ActivityMan.LoadAndLaunchGame(m_SaveGameName->GetText());

	if (success) {
		g_GUISound.ConfirmSound()->Play();
	} else {
		g_GUISound.UserErrorSound()->Play();
	}

	return success;
}

void SaveLoadMenuGUI::CreateSave() {
	bool success = g_ActivityMan.SaveCurrentGame(m_SaveGameName->GetText());
	if (success) {
		m_WasSaving = true;
	} else {
		g_GUISound.UserErrorSound()->Play();
	}
	m_SavingBlinkTimer.Reset();
	m_SaveGamesFetched = false;
}

void SaveLoadMenuGUI::DeleteSave() {
	std::string saveFilePath = g_PresetMan.GetFullModulePath(c_UserScriptedSavesModuleName) + "/" + m_SaveGameName->GetText() + ".ccsave";

	std::filesystem::remove(saveFilePath);
	g_GUISound.ConfirmSound()->Play();

	m_SaveGamesFetched = false;
}

void SaveLoadMenuGUI::UpdateButtonEnabledStates() {
	const bool isSaving = g_ActivityMan.IsCurrentlySaving();
	bool allowSave = !isSaving && g_ActivityMan.GetActivity() && g_ActivityMan.GetActivity()->GetAllowsUserSaving() && m_SaveGameName->GetText() != "";

	int existingSaveItemIndex = -1;
	for (int i = 0; i < m_SaveGamesListBox->GetItemList()->size(); ++i) {
		SaveRecord& save = m_SaveGames[m_SaveGamesListBox->GetItem(i)->m_ExtraIndex];
		if (save.SavePath.stem().string() == m_SaveGameName->GetText()) {
			existingSaveItemIndex = i;
			break;
		}
	}

	// Select the item in the list - selecting -1 unselects all
	m_SaveGamesListBox->SetSelectedIndex(existingSaveItemIndex);

	bool saveExists = existingSaveItemIndex != -1;

	bool allowCreate = allowSave && !saveExists;
	m_CreateButton->SetVisible(allowCreate);
	m_CreateButton->SetEnabled(allowCreate);

	bool allowOverwrite = allowSave && saveExists;
	m_OverwriteButton->SetVisible(allowOverwrite);
	m_OverwriteButton->SetEnabled(allowOverwrite);

	m_LoadButton->SetEnabled(saveExists && !isSaving);
	m_DeleteButton->SetEnabled(saveExists && !isSaving);
	
	m_DescriptionLabel->SetText("");

	if (isSaving != m_WasSaving) {
		m_SavingBlinkTimer.Reset();
		if (!isSaving) {
			if (g_ActivityMan.WaitForSaveGameTask()) g_GUISound.ConfirmSound()->Play();
			else g_GUISound.UserErrorSound()->Play();
		}
	}

	if (g_ActivityMan.GetActivity()) {
		if (isSaving) {
			const char* saveText = "";
			switch (m_SavingBlinkTimer.StepReal(500, 4)) {
				case 0:
					saveText = "Saving game, please wait   ";
					break;
				case 1:
					saveText = "Saving game, please wait.  ";
					break;
				case 2:
					saveText = "Saving game, please wait.. ";
					break;
				case 3:
					saveText = "Saving game, please wait...";
					break;
			}

			m_DescriptionLabel->SetText(saveText);
		} else if (g_ActivityMan.GetSaveGameTask().valid() && !m_SavingBlinkTimer.IsPastRealTimeLimit()) {
			m_DescriptionLabel->SetText(g_ActivityMan.WaitForSaveGameTask() ? "Game saved successfully!" : "Game could not be saved. See the console for details.");
		} else if (!g_ActivityMan.GetActivity()->GetAllowsUserSaving()) {
			m_DescriptionLabel->SetText("The currently played activity does not allow saving.");
		} else if (m_SaveGameName->GetText().empty()) {
			m_DescriptionLabel->SetText("Enter a name for your savegame.");
		}
	}

	m_WasSaving = isSaving;
}

void SaveLoadMenuGUI::SwitchToConfirmDialogMode(ConfirmDialogMode mode) {
	m_ConfirmDialogMode = mode;

	bool dialogOpen = m_ConfirmDialogMode != ConfirmDialogMode::None;
	m_SaveGameMenuBox->SetEnabled(!dialogOpen);
	m_ConfirmationBox->SetEnabled(dialogOpen);
	m_ConfirmationBox->SetVisible(dialogOpen);

	switch (m_ConfirmDialogMode) {
		case ConfirmDialogMode::ConfirmOverwrite:
			m_ConfirmationLabel->SetText("Are you sure you want to overwrite this savegame?");
			break;
		case ConfirmDialogMode::ConfirmDelete:
			m_ConfirmationLabel->SetText("Are you sure you want to delete this savegame?");
			break;
	}
}

bool SaveLoadMenuGUI::HandleInputEvents(PauseMenuGUI* pauseMenu) {
	PopulateSaveGamesList();
	
	m_GUIControlManager->Update();

	GUIEvent guiEvent;
	while (m_GUIControlManager->GetEvent(&guiEvent)) {
		if (guiEvent.GetType() == GUIEvent::Command) {
			if (guiEvent.GetControl() == m_BackToMainButton) {
				return true;
			} else if (guiEvent.GetControl() == m_LoadButton) {
				bool gameLoaded = LoadSave();
				if (gameLoaded) {
					return true;
				}
			} else if (guiEvent.GetControl() == m_CreateButton) {
				CreateSave();
			} else if (guiEvent.GetControl() == m_OverwriteButton) {
				SwitchToConfirmDialogMode(ConfirmDialogMode::ConfirmOverwrite);
			} else if (guiEvent.GetControl() == m_DeleteButton) {
				SwitchToConfirmDialogMode(ConfirmDialogMode::ConfirmDelete);
			} else if (guiEvent.GetControl() == m_ConfirmationButton) {
				switch (m_ConfirmDialogMode) {
					case ConfirmDialogMode::ConfirmOverwrite:
						CreateSave();
						break;
					case ConfirmDialogMode::ConfirmDelete:
						DeleteSave();
						break;
				}
				SwitchToConfirmDialogMode(ConfirmDialogMode::None);
			} else if (guiEvent.GetControl() == m_CancelButton) {
				SwitchToConfirmDialogMode(ConfirmDialogMode::None);
			}
		} else if (guiEvent.GetType() == GUIEvent::Notification) {
			if (guiEvent.GetMsg() == GUIButton::Focused && dynamic_cast<GUIButton*>(guiEvent.GetControl())) {
				g_GUISound.SelectionChangeSound()->Play();
			}

			if (guiEvent.GetControl() == m_SaveGamesListBox && (guiEvent.GetMsg() == GUIListBox::Select && m_SaveGamesListBox->GetSelectedIndex() > -1)) {
				const SaveRecord& record = m_SaveGames[m_SaveGamesListBox->GetSelected()->m_ExtraIndex];
				m_SaveGameName->SetText(record.SavePath.stem().string());
			}

			if (guiEvent.GetControl() == m_OrderByComboBox && guiEvent.GetMsg() == GUIComboBox::Closed) {
				UpdateSaveGamesGUIList();
			}
		}
	}

	UpdateButtonEnabledStates();

	return false;
}

void SaveLoadMenuGUI::Refresh() {
	m_SaveGamesFetched = false;
	UpdateButtonEnabledStates();
}

void SaveLoadMenuGUI::Draw() const {
	m_GUIControlManager->Draw();
}

void SaveLoadMenuGUI::RunCatalogSelfTest() {
	AllegroScreen screen(g_FrameMan.GetBackBuffer32());
	GUIInputWrapper input(-1, false);
	SaveLoadMenuGUI menu(&screen, &input, true);
	menu.PopulateSaveGamesList();
	for (const auto& record: menu.m_SaveGames) {
		std::cout << "[save-catalog] " << std::quoted(record.SavePath.stem().string()) << ' ' << std::quoted(record.Scene) << ' ' << std::quoted(record.Activity) << std::endl;
	}
	std::cout << "[save-catalog] complete=" << menu.m_SaveGamesFetched << " count=" << menu.m_SaveGames.size() << std::endl;
}

bool SaveLoadMenuGUI::RunSaveSelfTest(const std::string& name, bool& queued) {
	AllegroScreen screen(g_FrameMan.GetBackBuffer32());
	GUIInputWrapper input(-1, false);
	SaveLoadMenuGUI menu(&screen, &input, true);
	menu.m_SaveGameName->SetText(name);
	menu.PopulateSaveGamesList();
	menu.CreateSave();
	queued = menu.m_WasSaving;
	menu.UpdateButtonEnabledStates();
	const bool pendingControls = !menu.m_WasSaving || (!menu.m_CreateButton->GetEnabled() && !menu.m_OverwriteButton->GetEnabled() &&
	                                                 !menu.m_LoadButton->GetEnabled() && !menu.m_DeleteButton->GetEnabled());
	const bool pendingText = !menu.m_WasSaving || menu.m_DescriptionLabel->GetText().find("Saving game") == 0;
	const bool saved = g_ActivityMan.WaitForSaveGameTask();
	menu.UpdateButtonEnabledStates();
	const bool resultText = menu.m_DescriptionLabel->GetText() == (saved ? "Game saved successfully!" : "Game could not be saved. See the console for details.");
	const bool pass = pendingControls && pendingText && resultText;
	std::cout << "[save-menu-selftest] " << (pass ? "PASS" : "FAIL") << " controls=" << pendingControls << " pending_text=" << pendingText << " result_text=" << resultText << std::endl;
	return pass;
}
