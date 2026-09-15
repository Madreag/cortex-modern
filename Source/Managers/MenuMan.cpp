#include "MenuMan.h"
#include "SettingsMan.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "UInputMan.h"
#include "PresetMan.h"
#include "MetaMan.h"

#include "GUI.h"
#include "AllegroScreen.h"
#include "AllegroBitmap.h"
#include "GUIInputWrapper.h"

#include "Controller.h"
#include "TitleScreen.h"
#include "MainMenuGUI.h"
#include "ScenarioGUI.h"
#include "PauseMenuGUI.h"
#include "NetModerationGUI.h"
#include "MetagameGUI.h"
#include "LoadingScreen.h"
#include "System.h"
#include "NetMatchService.h"
#include "ScenarioRunner.h"

using namespace RTE;

void MenuMan::Initialize(bool firstTimeInit) {
	m_ActiveMenu = ActiveMenu::MenusDisabled;
	m_LocalPauseMenuOpening = false;

	m_GUIScreen = std::make_unique<AllegroScreen>(g_FrameMan.GetBackBuffer32());
	m_GUIInput = std::make_unique<GUIInputWrapper>(-1, g_UInputMan.GetJoystickCount() > 0);

	if (firstTimeInit) {
		m_IsInMenuScreen = false;
		m_LocalPauseMenuOpen = false;
		g_LoadingScreen.Create(m_GUIScreen.get(), m_GUIInput.get(), g_SettingsMan.GetLoadingScreenProgressReportDisabled());
	}

	m_TitleScreen = std::make_unique<TitleScreen>(m_GUIScreen.get());
	m_MainMenu = std::make_unique<MainMenuGUI>(m_GUIScreen.get(), m_GUIInput.get());
	m_ScenarioMenu = std::make_unique<ScenarioGUI>(m_GUIScreen.get(), m_GUIInput.get());
	m_PauseMenu = std::make_unique<PauseMenuGUI>(m_GUIScreen.get(), m_GUIInput.get());
	m_NetworkPanel = std::make_unique<NetModerationGUI>(m_GUIScreen.get());

	// TODO: MetaGameGUI doesn't seem to actually do anything with the Controller but removing conflicts with the second Create() method so that needs to be sorted out sometime in the year 3000.
	m_MenuController = std::make_unique<Controller>(Controller::CIM_PLAYER);
	g_MetaMan.GetGUI()->Create(m_MenuController.get());
}

void MenuMan::Reinitialize() {
	g_MetaMan.GetGUI()->Destroy();

	m_PauseMenu.reset();
	m_NetworkPanel.reset();
	m_ScenarioMenu.reset();
	m_MainMenu.reset();
	m_TitleScreen.reset();
	m_MenuController.reset();

	Initialize(false);
}

void MenuMan::SetActiveMenu() {
	ActiveMenu newActiveMenu = ActiveMenu::MenusDisabled;

	switch (m_TitleScreen->GetTitleTransitionState()) {
		case TitleScreen::TitleTransition::MainMenu:
		case TitleScreen::TitleTransition::MainMenuToCredits:
			newActiveMenu = ActiveMenu::MainMenuActive;
			break;
		case TitleScreen::TitleTransition::ScenarioMenu:
			newActiveMenu = ActiveMenu::ScenarioMenuActive;
			break;
		case TitleScreen::TitleTransition::MetaGameMenu:
			newActiveMenu = ActiveMenu::MetaGameMenuActive;
			break;
		case TitleScreen::TitleTransition::PauseMenu:
			newActiveMenu = ActiveMenu::PauseMenuActive;
			break;
		default:
			break;
	}

	if (newActiveMenu != m_ActiveMenu) {
		m_ActiveMenu = newActiveMenu;
		switch (m_ActiveMenu) {
			case ActiveMenu::MainMenuActive:
				// A finished online match comes in on its rematch lobby; a drop comes in on the rejoin
				// offer, which wins because its own routing sent us here.
				m_MainMenu->OfferRematchLobbyOnEntry();
				// §11: the rejoin offer is put up on the way in, at process start and after a match.
				m_MainMenu->OfferStoredRejoinOnEntry();
				break;
			case ActiveMenu::ScenarioMenuActive:
				m_ScenarioMenu->SetEnabled(m_TitleScreen->GetPlanetPos(), m_TitleScreen->GetPlanetRadius());
				break;
			case ActiveMenu::MetaGameMenuActive:
				g_MetaMan.GetGUI()->SetPlanetInfo(m_TitleScreen->GetPlanetPos(), m_TitleScreen->GetPlanetRadius());
				g_MetaMan.GetGUI()->SetEnabled();
				break;
			case ActiveMenu::PauseMenuActive:
				m_PauseMenu->EnableOrDisablePauseMenuFeatures();
				if (g_MetaMan.GameInProgress()) {
					m_PauseMenu->SetBackButtonTargetName("Conquest");
				} else {
					if (const Activity* activity = g_ActivityMan.GetActivity(); activity && activity->GetPresetName() == "None") {
						m_PauseMenu->SetBackButtonTargetName("Main");
					} else {
						m_PauseMenu->SetBackButtonTargetName("Scenario");
					}
				}
				break;
			default:
				break;
		}
	}
}

void MenuMan::SkipTitleIntroForAutomation() {
	m_TitleScreen->SkipIntro();
	SetActiveMenu();
}

void MenuMan::UpdateNetworkUI() { if (m_NetworkPanel) m_NetworkPanel->Update(); }
void MenuMan::DrawNetworkUI() const { if (m_NetworkPanel) m_NetworkPanel->Draw(); }
bool MenuMan::IsNetworkPanelOpen() const { return m_NetworkPanel && m_NetworkPanel->IsOpen(); }
bool MenuMan::ToggleNetworkPanel() { return m_NetworkPanel && m_NetworkPanel->SetOpen(!m_NetworkPanel->IsOpen()); }

bool MenuMan::ToggleLocalPauseMenu() {
	if (m_LocalPauseMenuOpen) {
		CloseLocalPauseMenu();
		return true;
	}
	// Only a running lockstep match has a session to keep running; everything else pauses as it always has.
	if (!m_PauseMenu || !ScenarioRunner::IsLockstepControllerSyncActive()) {
		return false;
	}
	m_LocalPauseMenuOpen = true;
	m_LocalPauseMenuOpening = true;
	m_PauseMenu->SetNetworkMatchMode(true);
	m_GUIInput->SetKeyJoyMouseCursor(true);
	g_UInputMan.TrapMousePos(false);
	return true;
}

void MenuMan::CloseLocalPauseMenu() {
	if (!m_LocalPauseMenuOpen) {
		return;
	}
	m_LocalPauseMenuOpen = false;
	m_LocalPauseMenuOpening = false;
	m_PauseMenu->SetNetworkMatchMode(false);
	// The edges the menu consumed are not the seat's to sample on the tick it closes.
	g_UInputMan.EndSimUpdate();
}

void MenuMan::RequestLocalPauseMenuBack() {
	if (m_LocalPauseMenuOpen) {
		m_PauseMenu->RequestBack();
	}
}

void MenuMan::UpdateLocalPauseMenu() {
	if (!m_LocalPauseMenuOpen) {
		return;
	}
	// The session keeps the menu, not the round: a resync stops the round for a moment with the match alive.
	const NetMatchServiceState serviceState = g_NetMatchService.GetState();
	const bool sessionLive = serviceState == NetMatchServiceState::Running || serviceState == NetMatchServiceState::Starting || serviceState == NetMatchServiceState::ReadyToLaunch;
	if (!sessionLive || !g_ActivityMan.IsInActivity()) {
		CloseLocalPauseMenu();
		return;
	}
	// A resolution change rebuilds the menu, and the mouse trap is reapplied every frame by the Activity.
	m_PauseMenu->SetNetworkMatchMode(true);
	g_UInputMan.TrapMousePos(false);
	if (m_LocalPauseMenuOpening) {
		m_LocalPauseMenuOpening = false;
		return;
	}
	switch (m_PauseMenu->Update()) {
		case PauseMenuGUI::PauseMenuUpdateResult::ActivityResumed:
			CloseLocalPauseMenu();
			break;
		case PauseMenuGUI::PauseMenuUpdateResult::MatchLeft:
			CloseLocalPauseMenu();
			// The leave itself is the running-service path the game loop takes when the activity is no longer in play.
			g_ActivityMan.PauseActivity(true, true);
			break;
		default:
			break;
	}
}

void MenuMan::DrawLocalPauseMenu() const {
	if (m_LocalPauseMenuOpen) {
		m_PauseMenu->Draw(false);
	}
}

void MenuMan::HandleTransitionIntoMenuLoop() {
	// Whatever sends us to the menus ends the match this menu was local to.
	CloseLocalPauseMenu();
	// §11: a match this peer was dropped from sends the player to the main menu, where the rejoin
	// offer and the retry status are, rather than to the planet screen the preset would pick.
	if (g_NetMatchService.NeedsRecoveryPump()) {
		m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::ScrollingFadeIn);
		return;
	}
	// An online match that ended with its session alive reconvenes in the rematch lobby, so both
	// peers land on the multiplayer screen rather than on the planet screen the preset would pick.
	if (g_NetMatchService.NeedsCompletedLobbyPump()) {
		m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::ScrollingFadeIn);
		return;
	}
	if (g_MetaMan.GameInProgress()) {
		if (g_ActivityMan.SkipPauseMenuWhenPausingActivity()) {
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::MetaGameFadeIn);
		} else {
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::PauseMenu);
		}
	} else if (!g_ActivityMan.ActivitySetToRestart()) {
		if (const Activity* activity = g_ActivityMan.GetActivity(); activity) {
			if (activity->GetPresetName() == "None") {
				// If we're in the editors or in online multiplayer then return to main menu instead of scenario menu.
				m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::ScrollingFadeIn);
			} else {
				if (activity->IsOver() || g_ActivityMan.SkipPauseMenuWhenPausingActivity()) {
					m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::ScenarioFadeIn);
				} else {
					m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::PauseMenu);
				}
			}
		} else {
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::ScrollingFadeIn);
		}
	}
}

bool MenuMan::Update() {
	// §11's retry schedule belongs to the service, not to a screen: it runs whatever menu is up, so a
	// dropped player recovers without having to walk back to the multiplayer screen.
	// A completed match's rematch lobby needs the same pump: its directory lease heartbeats from here.
	if (g_NetMatchService.NeedsRecoveryPump() || g_NetMatchService.NeedsCompletedLobbyPump()) {
		g_NetMatchService.Update();
	}
	m_TitleScreen->Update();
	SetActiveMenu();

	bool quitResult = false;

	switch (m_ActiveMenu) {
		case ActiveMenu::MainMenuActive:
			quitResult = UpdateMainMenu();
			break;
		case ActiveMenu::ScenarioMenuActive:
			UpdateScenarioMenu();
			break;
		case ActiveMenu::MetaGameMenuActive:
			quitResult = UpdateMetaGameMenu();
			break;
		case ActiveMenu::PauseMenuActive:
			UpdatePauseMenu();
			break;
		default:
			break;
	}
	if (quitResult) {
		m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::ScrollingFadeOutQuit);
	} else if (m_TitleScreen->GetTitleTransitionState() != TitleScreen::TitleTransition::ScrollingFadeOutQuit) {
		m_GUIInput->SetKeyJoyMouseCursor(true);
	}
	if (m_TitleScreen->GetTitleTransitionState() == TitleScreen::TitleTransition::TransitionEndQuit) {
		System::SetQuit();
	} else if (m_TitleScreen->GetTitleTransitionState() == TitleScreen::TitleTransition::TransitionEnd) {
		m_TitleScreen->SetTitlePendingTransition();
		return true;
	}
	return false;
}

bool MenuMan::UpdateMainMenu() const {
	switch (m_MainMenu->Update()) {
		case MainMenuGUI::MainMenuUpdateResult::MetaGameStarted:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::MainMenuToMetaGame);
			break;
		case MainMenuGUI::MainMenuUpdateResult::ScenarioStarted:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::MainMenuToScenario);
			break;
		case MainMenuGUI::MainMenuUpdateResult::EnterCreditsScreen:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::MainMenuToCredits);
			break;
		case MainMenuGUI::MainMenuUpdateResult::BackToMainFromCredits:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::CreditsToMainMenu);
			break;
		case MainMenuGUI::MainMenuUpdateResult::ActivityStarted:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::ScrollingFadeOut);
			g_ActivityMan.SetRestartActivity();
			break;
		case MainMenuGUI::MainMenuUpdateResult::ActivityResumed:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::TransitionEnd);
			g_ActivityMan.SetResumeActivity();
			break;
		case MainMenuGUI::MainMenuUpdateResult::Quit:
			return true;
		default:
			break;
	}
	return false;
}

void MenuMan::UpdateScenarioMenu() const {
	switch (m_ScenarioMenu->Update()) {
		case ScenarioGUI::ScenarioMenuUpdateResult::BackToMain:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::PlanetToMainMenu);
			break;
		case ScenarioGUI::ScenarioMenuUpdateResult::ActivityResumed:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::TransitionEnd);
			g_ActivityMan.SetResumeActivity();
			break;
		case ScenarioGUI::ScenarioMenuUpdateResult::ActivityStarted:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::FadeOut);
			if (g_MetaMan.GameInProgress()) {
				g_MetaMan.EndGame();
			}
			g_ActivityMan.SetRestartActivity();
			break;
		default:
			break;
	}
}

bool MenuMan::UpdateMetaGameMenu() const {
	g_MetaMan.GetGUI()->SetStationOrbitPos(m_TitleScreen->GetStationPos());
	g_MetaMan.Update();

	if (g_MetaMan.GetGUI()->BackToMain()) {
		m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::PlanetToMainMenu);
	} else if (g_MetaMan.GetGUI()->ActivityRestarted()) {
		m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::FadeOut);
		g_ActivityMan.SetRestartActivity();
	} else if (g_MetaMan.GetGUI()->ActivityResumed()) {
		m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::TransitionEnd);
		g_ActivityMan.SetResumeActivity();
	}

	return g_MetaMan.GetGUI()->QuitProgram();
}

void MenuMan::UpdatePauseMenu() const {
	switch (m_PauseMenu->Update()) {
		case PauseMenuGUI::PauseMenuUpdateResult::ActivityResumed:
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::TransitionEnd);
			g_ActivityMan.SetResumeActivity(true);
			break;
		case PauseMenuGUI::PauseMenuUpdateResult::BackToMain:
			m_TitleScreen->SetTitleTransitionState(g_MetaMan.GameInProgress() ? TitleScreen::TitleTransition::MetaGameFadeIn : TitleScreen::TitleTransition::ScenarioFadeIn);
			break;
		case PauseMenuGUI::PauseMenuUpdateResult::MatchLeft:
			// The match rows normally show only under the local pause menu; leave the service the way the game loop does.
			if (g_NetMatchService.GetState() == NetMatchServiceState::Running) {
				g_NetMatchService.LeaveMatch("Match left");
			}
			m_TitleScreen->SetTitleTransitionState(TitleScreen::TitleTransition::ScrollingFadeIn);
			break;
		default:
			break;
	}
}

void MenuMan::Draw() const {
	g_FrameMan.ClearBackBuffer32();

	// Early return when changing resolution so screen remains black while everything is being recreated instead of being stuck showing a badly aligned title screen.
	if (g_WindowMan.ResolutionChanged()) {
		return;
	}

	switch (m_ActiveMenu) {
		case ActiveMenu::MainMenuActive:
			m_TitleScreen->Draw();
			m_MainMenu->Draw();
			break;
		case ActiveMenu::ScenarioMenuActive:
			m_TitleScreen->Draw();
			m_ScenarioMenu->Draw();
			break;
		case ActiveMenu::MetaGameMenuActive:
			m_TitleScreen->Draw();
			g_MetaMan.Draw(g_FrameMan.GetBackBuffer32());
			break;
		case ActiveMenu::PauseMenuActive:
			m_PauseMenu->Draw();
			break;
		default:
			m_TitleScreen->Draw();
			break;
	}
	if (m_ActiveMenu != ActiveMenu::MenusDisabled && g_UInputMan.GetJoystickCount() > 0) {
		int device = g_UInputMan.GetLastDeviceWhichControlledGUICursor();

		// Draw the active joystick's sprite next to the mouse.
		if (device >= InputDevice::DEVICE_GAMEPAD_1) {
			int mouseX = 0;
			int mouseY = 0;
			m_GUIInput->GetMousePosition(&mouseX, &mouseY);
			BITMAP* deviceIcon = g_UInputMan.GetDeviceIcon(device)->GetBitmaps32()[0];
			if (deviceIcon) {
				draw_sprite(g_FrameMan.GetBackBuffer32(), deviceIcon, mouseX + (deviceIcon->w / 2), mouseY - (deviceIcon->h / 5));
			}
		}
		// Show which joysticks are detected by the game.
		for (int playerIndex = Players::PlayerOne; playerIndex < Players::MaxPlayerCount; playerIndex++) {
			if (g_UInputMan.JoystickActive(playerIndex)) {
				int matchedDevice = InputDevice::DEVICE_GAMEPAD_1 + playerIndex;
				if (matchedDevice != device) {
					BITMAP* deviceIcon = g_UInputMan.GetDeviceIcon(matchedDevice)->GetBitmaps32()[0];
					if (deviceIcon) {
						draw_sprite(g_FrameMan.GetBackBuffer32(), deviceIcon, g_WindowMan.GetResX() - 30 * g_UInputMan.GetJoystickCount() + 30 * playerIndex, g_WindowMan.GetResY() - 25);
					}
				}
			}
		}
	}
}
