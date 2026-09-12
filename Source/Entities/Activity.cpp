#include "CheckpointArchive.h"
#include "Base64/base64.h"
#include "Activity.h"
#include "GameActivity.h"

#include "CameraMan.h"
#include "PresetMan.h"
#include "MovableMan.h"
#include "UInputMan.h"
#include "WindowMan.h"
#include "FrameMan.h"
#include "MetaMan.h"
#include "SceneMan.h"
#include "ScenarioRunner.h"
#include "NetActorOwnership.h"
#include "LuaMan.h"

#include "ACraft.h"

#include "GUI.h"
#include "GUIFont.h"
#include "AllegroBitmap.h"

#include "RTETools.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <iostream>

using namespace RTE;

AbstractClassInfo(Activity, Entity);

Activity::Activity() {
	Clear();
}

Activity::~Activity() {
	Destroy(true);
}

void Activity::Clear() {
	m_PendingRuntimeCheckpoint.clear();
	m_CheckpointActorIDs = {};
	m_HasCheckpointActorIDs = false;
	m_ActivityState = ActivityState::NotStarted;
	m_Paused = false;
	m_AllowsUserSaving = false;
	m_IsTestActivity = false;
	m_Description.clear();
	m_SceneName.clear();
	m_MaxPlayerSupport = Players::MaxPlayerCount;
	m_MinTeamsRequired = 2;
	m_Difficulty = 50;
	m_CraftOrbitAtTheEdge = false;
	m_InCampaignStage = -1;
	m_PlayerCount = 1;
	m_TeamCount = 1;

	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		// Player 1 is active by default, for the editor etc
		m_IsActive[player] = player == Players::PlayerOne;
		m_IsHuman[player] = player == Players::PlayerOne;
		m_PlayerScreen[player] = (player == Players::PlayerOne) ? Players::PlayerOne : Players::NoPlayer;
		m_ViewState[player] = ViewState::Normal;
		m_DeathTimer[player].Reset();
		m_Team[player] = Teams::TeamOne;
		m_TeamFundsShare[player] = 1.0F;
		m_FundsContribution[player] = 0;
		m_Brain[player] = 0;
		m_HadBrain[player] = false;
		m_BrainEvacuated[player] = false;
		m_ControlledActor[player] = 0;
		m_PlayerController[player].Reset();
		m_MessageTimer[player].Reset();
	}

	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		m_TeamNames[team] = "Team " + std::to_string(team + 1);
		m_TeamIcons[team].Reset();
		// Team 1 is active by default, for the editors etc
		m_TeamActive[team] = team == Teams::TeamOne;
		m_TeamDeaths[team] = 0;
		m_TeamAISkillLevels[team] = AISkillSetting::DefaultSkill;
		m_TeamFunds[team] = 2000;
		m_FundsChanged[team] = false;
	}

	m_SavedValues.Reset();
}

int Activity::Create() {
	if (Entity::Create() < 0) {
		return -1;
	}
	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		std::string teamNum = std::to_string(team + 1);
		m_TeamIcons[team] = *dynamic_cast<const Icon*>(g_PresetMan.GetEntityPreset("Icon", "Team " + teamNum + " Default"));
	}
	return 0;
}

int Activity::Create(const Activity& reference) {
	Entity::Create(reference);

	m_ActivityState = reference.m_ActivityState;
	m_Paused = reference.m_Paused;
	m_AllowsUserSaving = reference.m_AllowsUserSaving;
	m_Description = reference.m_Description;
	m_SceneName = reference.m_SceneName;
	m_IsTestActivity = reference.m_IsTestActivity;
	m_MaxPlayerSupport = reference.m_MaxPlayerSupport;
	m_MinTeamsRequired = reference.m_MinTeamsRequired;
	m_Difficulty = reference.m_Difficulty;
	m_CraftOrbitAtTheEdge = reference.m_CraftOrbitAtTheEdge;
	m_InCampaignStage = reference.m_InCampaignStage;
	m_PlayerCount = reference.m_PlayerCount;
	m_TeamCount = reference.m_TeamCount;

	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		m_Team[player] = reference.m_Team[player];
		m_FundsContribution[player] = reference.m_FundsContribution[player];
		m_TeamFundsShare[player] = reference.m_TeamFundsShare[player];
		m_IsActive[player] = reference.m_IsActive[player];
		m_IsHuman[player] = reference.m_IsHuman[player];
		m_PlayerScreen[player] = reference.m_PlayerScreen[player];
		m_Brain[player] = reference.m_Brain[player];
		m_HadBrain[player] = reference.m_HadBrain[player];
		m_BrainEvacuated[player] = reference.m_BrainEvacuated[player];
		m_ViewState[player] = reference.m_ViewState[player];
		m_ControlledActor[player] = reference.m_ControlledActor[player];
		m_PlayerController[player] = reference.m_PlayerController[player];
	}

	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		m_TeamActive[team] = reference.m_TeamActive[team];
		m_TeamFunds[team] = reference.m_TeamFunds[team];
		m_FundsChanged[team] = reference.m_FundsChanged[team];
		m_TeamNames[team] = reference.m_TeamNames[team];
		if (reference.m_TeamIcons[team].GetFrameCount() > 0) {
			m_TeamIcons[team] = reference.m_TeamIcons[team];
		}
		m_TeamDeaths[team] = reference.m_TeamDeaths[team];
		m_TeamAISkillLevels[team] = reference.m_TeamAISkillLevels[team];
	}

	m_SavedValues = reference.m_SavedValues;
	m_PendingRuntimeCheckpoint = reference.m_PendingRuntimeCheckpoint;
	m_CheckpointActorIDs = reference.m_CheckpointActorIDs;
	m_HasCheckpointActorIDs = reference.m_HasCheckpointActorIDs;
	if (MovableObject::IsFaithfulClone() && !Activity::LoadCheckpoint(reference.Activity::SaveCheckpoint())) return -1;

	return 0;
}

int Activity::ReadProperty(const std::string_view& propName, Reader& reader) {
	StartPropertyList(return Entity::ReadProperty(propName, reader));

	MatchProperty("Description", { reader >> m_Description; });
	MatchProperty("SceneName", { reader >> m_SceneName; });
	MatchProperty("MaxPlayerSupport", { reader >> m_MaxPlayerSupport; });
	MatchProperty("MinTeamsRequired", { reader >> m_MinTeamsRequired; });
	MatchProperty("Difficulty", { reader >> m_Difficulty; });
	MatchProperty("CraftOrbitAtTheEdge", { reader >> m_CraftOrbitAtTheEdge; });
	MatchProperty("InCampaignStage", { reader >> m_InCampaignStage; });
	MatchProperty("ActivityState", { m_ActivityState = static_cast<ActivityState>(std::stoi(reader.ReadPropValue())); });
	MatchProperty("AllowsUserSaving", { reader >> m_AllowsUserSaving; });
	MatchProperty("IsTestActivity", { reader >> m_IsTestActivity; });
	MatchForwards("TeamOfPlayer1") MatchForwards("TeamOfPlayer2") MatchForwards("TeamOfPlayer3") MatchProperty("TeamOfPlayer4", {
		for (int playerTeam = Teams::TeamOne; playerTeam < Teams::MaxTeamCount; playerTeam++) {
			std::string playerTeamNum = std::to_string(playerTeam + 1);
			if (propName == "TeamOfPlayer" + playerTeamNum) {
				int team;
				reader >> team;
				if (team >= Teams::TeamOne && team < Teams::MaxTeamCount) {
					m_Team[playerTeam] = team;
					m_IsActive[playerTeam] = true;
					if (!m_TeamActive[team]) {
						m_TeamCount++;
					}
					m_TeamActive[team] = true;
				} else {
					m_IsActive[playerTeam] = false;
				}
				break;
			}
		}
	});
	MatchForwards("Player1IsHuman") MatchForwards("Player2IsHuman") MatchForwards("Player3IsHuman") MatchProperty("Player4IsHuman", {
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; player++) {
			std::string playerNum = std::to_string(player + 1);
			if (propName == "Player" + playerNum + "IsHuman") {
				reader >> m_IsHuman[player];
				break;
			}
		}
	});
	MatchForwards("Team1Name") MatchForwards("Team2Name") MatchForwards("Team3Name") MatchProperty("Team4Name", {
		for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; team++) {
			std::string teamNum = std::to_string(team + 1);
			if (propName == "Team" + teamNum + "Name") {
				reader >> m_TeamNames[team];
				if (!m_TeamActive[team]) {
					m_TeamCount++;
				}
				m_TeamActive[team] = true;
				break;
			}
		}
	});
	MatchForwards("Team1Icon") MatchForwards("Team2Icon") MatchForwards("Team3Icon") MatchProperty("Team4Icon", {
		for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; team++) {
			std::string teamNum = std::to_string(team + 1);
			if (propName == "Team" + teamNum + "Icon") {
				reader >> m_TeamIcons[team];
				break;
			}
		}
	});
	MatchForwards("Team1Funds") MatchForwards("Team2Funds") MatchForwards("Team3Funds") MatchProperty("Team4Funds", {
		for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; team++) {
			std::string teamNum = std::to_string(team + 1);
			if (propName == "Team" + teamNum + "Funds") {
				reader >> m_TeamFunds[team];
				break;
			}
		}
	});
	MatchForwards("TeamFundsShareOfPlayer1") MatchForwards("TeamFundsShareOfPlayer2") MatchForwards("TeamFundsShareOfPlayer3") MatchProperty("TeamFundsShareOfPlayer4", {
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; player++) {
			std::string playerNum = std::to_string(player + 1);
			if (propName == "TeamFundsShareOfPlayer" + playerNum) {
				reader >> m_TeamFundsShare[player];
				break;
			}
		}
	});
	MatchForwards("FundsContributionOfPlayer1") MatchForwards("FundsContributionOfPlayer2") MatchForwards("FundsContributionOfPlayer3") MatchProperty("FundsContributionOfPlayer4", {
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; player++) {
			std::string playerNum = std::to_string(player + 1);
			if (propName == "FundsContributionOfPlayer" + playerNum) {
				reader >> m_FundsContribution[player];
				break;
			}
		}
	});
	MatchProperty("GenericSavedValues", { reader >> m_SavedValues; });
	MatchProperty("SpecialBehaviour_RuntimeCheckpoint", { m_PendingRuntimeCheckpoint = base64_decode(reader.ReadPropValue()); });

	EndPropertyList;
}

int Activity::Save(Writer& writer) const {
	Entity::Save(writer);
	if (writer.IsSnapshot()) writer.NewPropertyWithValue("SpecialBehaviour_RuntimeCheckpoint", base64_encode(m_PendingRuntimeCheckpoint.empty() ? SaveCheckpoint() : m_PendingRuntimeCheckpoint, true));

	writer.NewProperty("Description");
	writer << m_Description;
	writer.NewProperty("IsTestActivity");
	writer << m_IsTestActivity;
	writer.NewProperty("SceneName");
	writer << m_SceneName;
	writer.NewProperty("MaxPlayerSupport");
	writer << m_MaxPlayerSupport;
	writer.NewProperty("MinTeamsRequired");
	writer << m_MinTeamsRequired;
	writer.NewProperty("Difficulty");
	writer << m_Difficulty;
	writer.NewProperty("CraftOrbitAtTheEdge");
	writer << m_CraftOrbitAtTheEdge;
	writer.NewProperty("InCampaignStage");
	writer << m_InCampaignStage;
	writer.NewProperty("ActivityState");
	writer << m_ActivityState;
	writer.NewProperty("AllowsUserSaving");
	writer << m_AllowsUserSaving;

	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; player++) {
		std::string playerNum = std::to_string(player + 1);
		if (m_IsActive[player]) {
			writer.NewProperty("TeamOfPlayer" + playerNum);
			writer << m_Team[player];
			writer.NewProperty("FundsContributionOfPlayer" + playerNum);
			writer << m_FundsContribution[player];
			writer.NewProperty("TeamFundsShareOfPlayer" + playerNum);
			writer << m_TeamFundsShare[player];
			writer.NewProperty("Player" + playerNum + "IsHuman");
			writer << m_IsHuman[player];
		} else {
			writer.NewProperty("TeamOfPlayer" + playerNum);
			writer << Teams::NoTeam;
		}
	}

	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; team++) {
		std::string teamNum = std::to_string(team + 1);
		if (m_TeamActive[team]) {
			writer.NewProperty("Team" + teamNum + "Funds");
			writer << m_TeamFunds[team];
			writer.NewProperty("Team" + teamNum + "Name");
			writer << m_TeamNames[team];
			writer.NewProperty("Team" + teamNum + "Icon");
			m_TeamIcons[team].SavePresetCopy(writer);
		}
	}

	writer.NewProperty("GenericSavedValues");
	writer << m_SavedValues;

	return 0;
}

int Activity::Start() {
	// Reseed the RNG for determinism
	SeedRNG();
	// Reseed each Lua state's math.random from the same sim seed so script RNG is deterministic per activity.
	g_LuaMan.SeedAllLuaRNGs(g_SimRNG.GetSeed());

	if (m_ActivityState != ActivityState::Editing) {
		m_ActivityState = ActivityState::Running;
	}
	m_Paused = false;

	// Reset the mouse moving so that it won't trap the mouse if the window isn't in focus (common after loading)
	if (!g_MovableMan.IsRestoringSnapshot()) {
		g_UInputMan.DisableMouseMoving(true);
		g_UInputMan.DisableMouseMoving(false);
		g_UInputMan.DisableKeys(false);
	}

	int error = g_SceneMan.LoadScene();
	if (error < 0) {
		return error;
	}

	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		if (!m_TeamActive[team]) {
			continue;
		}
		m_FundsChanged[team] = false;
		m_TeamDeaths[team] = 0;
	}

	// Intentionally doing all players, all need controllers
	std::vector<int> playerControlled;
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		m_ViewState[player] = ViewState::Normal;

		m_PlayerController[player].Reset();
		m_PlayerController[player].Create(Controller::CIM_PLAYER, player);
		m_PlayerController[player].SetTeam(m_Team[player]);

		if (m_IsHuman[player]) {
			playerControlled.push_back(player);
		}
		m_MessageTimer[player].Reset();

		if (int screenId = ScreenOfPlayer(player); screenId != -1) {
			g_FrameMan.ClearScreenText(screenId);
			g_CameraMan.SetScreenOcclusion(Vector(), screenId);
		}
		g_CameraMan.ResetAllScreenShake();

		// TODO currently this sets brains to players arbitrarily. We should save information on which brain is for which player in the scene so we can set them properly!
		if (m_IsActive[player]) {
			if (Actor* brain = g_MovableMan.GetUnassignedBrain(GetTeamOfPlayer(player))) {
				SetPlayerBrain(brain, player);
			}
		}
	}

	if (!g_MovableMan.IsRestoringSnapshot()) g_UInputMan.CheckMultiMouseKeyboardEnabled(playerControlled);

	return 0;
}

void Activity::End() {
	g_AudioMan.FinishIngameLoopingSounds();
	// Actor control is automatically disabled when players are set to observation mode, so no need to do anything directly.
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		m_ViewState[player] = ViewState::Observe;
	}
	m_ActivityState = ActivityState::Over;
}

void Activity::SetupPlayers() {
	m_TeamCount = 0;
	m_PlayerCount = 0;

	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		m_TeamActive[team] = false;
	}

	// Local players only cover this peer's team; a lockstep match must run the synced roster's
	// full team set on every peer or the sims diverge on the first TeamActive read.
	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		if (ScenarioRunner::IsLockstepActiveTeam(team)) {
			m_TeamActive[team] = true;
			m_TeamCount++;
		}
	}

	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (m_IsActive[player]) {
			m_PlayerCount++;
			if (!m_TeamActive[m_Team[player]]) {
				m_TeamCount++;
			}
			m_TeamActive[m_Team[player]] = true;
		}

		// Calculate which screen each human player is using, based on how many non-human players there are before him
		int screenIndex = -1;
		if (m_IsActive[player] && m_IsHuman[player]) {
			for (int playerToCheck = Players::PlayerOne; playerToCheck < Players::MaxPlayerCount && playerToCheck <= player; ++playerToCheck) {
				if (m_IsActive[playerToCheck] && m_IsHuman[playerToCheck]) {
					screenIndex++;
				}
			}
		}
		m_PlayerScreen[player] = screenIndex;
	}
}

bool Activity::DeactivatePlayer(int playerToDeactivate) {
	if (playerToDeactivate < Players::PlayerOne || playerToDeactivate >= Players::MaxPlayerCount || !m_IsActive[playerToDeactivate] || !m_TeamActive[m_Team[playerToDeactivate]]) {
		return false;
	}

	// We need to check if this player is the last one on their team and, if so, deactivate the team
	bool lastOnTeam = true;

	for (int playerToCheck = Players::PlayerOne; playerToCheck < Players::MaxPlayerCount; ++playerToCheck) {
		if (m_IsActive[playerToCheck] && playerToCheck != playerToDeactivate && m_Team[playerToCheck] == m_Team[playerToDeactivate]) {
			lastOnTeam = false;
			break;
		}
	}
	if (lastOnTeam) {
		m_TeamActive[m_Team[playerToDeactivate]] = false;
		m_TeamCount--;
	}
	m_IsActive[playerToDeactivate] = false;
	m_PlayerCount--;

	return true;
}

int Activity::AddPlayer(int playerToAdd, bool isHuman, int team, float funds, const Icon* teamIcon) {
	if (playerToAdd < Players::PlayerOne || playerToAdd >= Players::MaxPlayerCount || team < Teams::TeamOne || team >= Teams::MaxTeamCount) {
		return m_PlayerCount;
	}

	if (!m_TeamActive[team]) {
		m_TeamFundsShare[playerToAdd] = 1.0F;
		m_TeamCount++;
	} else {
		float totalFunds = m_TeamFunds[team] + funds;
		float newRatio = 1.0F + (funds / totalFunds);

		for (int teamPlayer = Players::PlayerOne; teamPlayer < Players::MaxPlayerCount; ++teamPlayer) {
			if (m_IsActive[teamPlayer] && m_Team[teamPlayer] == team) {
				m_TeamFundsShare[teamPlayer] /= newRatio;
			}
		}
		m_TeamFundsShare[playerToAdd] = funds / totalFunds;
	}

	m_TeamActive[team] = true;
	if (teamIcon) {
		SetTeamIcon(team, *teamIcon);
	}

	if (!m_IsActive[playerToAdd]) {
		m_PlayerCount++;
	}
	m_IsActive[playerToAdd] = true;

	m_IsHuman[playerToAdd] = isHuman;
	m_Team[playerToAdd] = team;
	m_TeamFunds[team] += funds;
	m_FundsContribution[playerToAdd] = funds;

	return m_PlayerCount;
}

void Activity::ClearPlayers(bool resetFunds) {
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		m_IsActive[player] = false;
		m_IsHuman[player] = false;

		if (resetFunds) {
			m_FundsContribution[player] = 0;
			m_TeamFundsShare[player] = 1.0F;
		}
	}

	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		m_TeamActive[team] = false;
		if (resetFunds) {
			m_TeamFunds[team] = 0;
		}
	}

	m_PlayerCount = m_TeamCount = 0;
}

uint8_t Activity::GetHumanCount() const {
	uint8_t humans = 0;
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (m_IsActive[player] && m_IsHuman[player]) {
			humans++;
		}
	}
	return humans;
}

void Activity::SetTeamOfPlayer(int player, int team) {
	if (team < Teams::TeamOne || team >= Teams::MaxTeamCount || player < Players::PlayerOne || player >= Players::MaxPlayerCount) {
		return;
	}

	m_Team[player] = team;
	m_TeamActive[team] = true;
	m_IsActive[player] = true;
}

int Activity::PlayerOfScreen(int screen) const {
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (m_PlayerScreen[player] == screen) {
			return player;
		}
	}
	return Players::NoPlayer;
}

std::string Activity::GetTeamName(int whichTeam) const {
	if (whichTeam >= Teams::TeamOne && whichTeam < Teams::MaxTeamCount) {
		return m_TeamActive[whichTeam] ? m_TeamNames[whichTeam] : "Inactive Team";
	}
	return "";
}

bool Activity::IsHumanTeam(int whichTeam) const {
	// Local player bindings are per-peer in a lockstep match; the synced match config owns team
	// humanity so sim mutations gated on it (AI jetpack fuel, refunds) are identical on every peer.
	if (ScenarioRunner::IsLockstepControllerSyncActive()) {
		return ScenarioRunner::IsLockstepHumanTeam(whichTeam);
	}
	if (whichTeam >= Teams::TeamOne && whichTeam < Teams::MaxTeamCount) {
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
			if (m_IsActive[player] && m_Team[player] == whichTeam && m_IsHuman[player]) {
				return true;
			}
		}
	}
	return false;
}

int Activity::PlayersInTeamCount(int team) const {
	int count = 0;
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (m_IsActive[player] && m_Team[player] == team) {
			count++;
		}
	}
	return count;
}

void Activity::ChangeTeamFunds(float howMuch, int whichTeam) {
	if (whichTeam >= Teams::TeamOne && whichTeam < Teams::MaxTeamCount) {
		m_TeamFunds[whichTeam] += howMuch;
		m_FundsChanged[whichTeam] = true;
		if (IsHumanTeam(whichTeam)) {
			for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; player++) {
				if (m_Team[player] == whichTeam) {
					g_GUISound.FundsChangedSound()->Play(player);
				}
			}
		}
	}
}

bool Activity::TeamFundsChanged(int whichTeam) {
	if (whichTeam >= Teams::TeamOne && whichTeam < Teams::MaxTeamCount) {
		bool changed = m_FundsChanged[whichTeam];
		m_FundsChanged[whichTeam] = false;
		return changed;
	}
	return false;
}

bool Activity::UpdatePlayerFundsContribution(int player, float newFunds) {
	if (player < Players::PlayerOne || player >= Players::MaxPlayerCount || !m_IsActive[player] || !m_TeamActive[m_Team[player]]) {
		return false;
	}

	// If this guy is responsible for all funds, then just update his contribution and be done
	if (m_TeamFundsShare[player] == 1.0F) {
		m_FundsContribution[player] = newFunds;
		m_TeamFunds[m_Team[player]] = newFunds;
	} else {
		m_FundsContribution[player] = newFunds;

		// Tally up all the funds of all players on this guy's team
		for (int playerOnTeam = Players::PlayerOne; playerOnTeam < Players::MaxPlayerCount; ++playerOnTeam) {
			if (m_IsActive[playerOnTeam] && m_Team[playerOnTeam] == m_Team[player]) {
				m_TeamFunds[m_Team[player]] += m_FundsContribution[playerOnTeam];
			}
		}
		// Now that we have the updated total, update the shares of all team players
		for (int playerOnTeam = Players::PlayerOne; playerOnTeam < Players::MaxPlayerCount; ++playerOnTeam) {
			if (m_IsActive[playerOnTeam] && m_Team[playerOnTeam] == m_Team[player]) {
				m_TeamFundsShare[playerOnTeam] = m_FundsContribution[playerOnTeam] / m_TeamFunds[m_Team[player]];
			}
		}
	}
	return true;
}

float Activity::GetPlayerFundsShare(int player) const {
	if (player >= Players::PlayerOne && player < Players::MaxPlayerCount) {
		return (m_FundsContribution[player] > 0.0F) ? (m_TeamFunds[m_Team[player]] * m_TeamFundsShare[player]) : 0.0F;
	}
	return 0;
}

void Activity::SetPlayerBrain(Actor* newBrain, int player) {
	if ((player >= Players::PlayerOne && player < Players::MaxPlayerCount) && newBrain) {
		if (newBrain->GetTeam() != m_Team[player]) {
			newBrain->SetTeam(m_Team[player]);
		}
		m_HadBrain[player] = true;
	}
	m_Brain[player] = newBrain;
}

bool Activity::AnyBrainWasEvacuated() const {
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (m_BrainEvacuated[player]) {
			return true;
		}
	}
	return false;
}

bool Activity::IsAssignedBrain(Actor* actor) const {
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (actor == m_Brain[player]) {
			return true;
		}
	}
	return false;
}

int Activity::IsBrainOfWhichPlayer(Actor* actor) const {
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (actor == m_Brain[player]) {
			return player;
		}
	}
	return Players::NoPlayer;
}

bool Activity::IsOtherPlayerBrain(Actor* actor, int player) const {
	for (int playerToCheck = Players::PlayerOne; playerToCheck < Players::MaxPlayerCount; ++playerToCheck) {
		if (m_IsActive[playerToCheck] && playerToCheck != player && actor == m_Brain[playerToCheck]) {
			return true;
		}
	}
	return false;
}

std::string Activity::GetDifficultyString(int difficulty) {
	if (difficulty <= DifficultySetting::CakeDifficulty) {
		return "Cake";
	} else if (difficulty <= DifficultySetting::EasyDifficulty) {
		return "Easy";
	} else if (difficulty <= DifficultySetting::MediumDifficulty) {
		return "Medium";
	} else if (difficulty <= DifficultySetting::HardDifficulty) {
		return "Hard";
	} else if (difficulty <= DifficultySetting::NutsDifficulty) {
		return "Nuts";
	} else {
		return "Nuts!";
	}
}

std::string Activity::GetAISkillString(int skill) {
	if (skill < AISkillSetting::InferiorSkill) {
		return "Inferior";
	} else if (skill < AISkillSetting::AverageSkill) {
		return "Average";
	} else if (skill < AISkillSetting::GoodSkill) {
		return "Good";
	} else {
		return "Unfair";
	}
}

int Activity::GetTeamAISkill(int team) const {
	if (team >= Teams::TeamOne && team < Teams::MaxTeamCount) {
		return m_TeamAISkillLevels[team];
	} else {
		int avgskill = 0;
		int count = 0;

		for (int playerTeam = Teams::TeamOne; playerTeam < Teams::MaxTeamCount; playerTeam++) {
			if (TeamActive(playerTeam)) {
				avgskill += GetTeamAISkill(playerTeam);
				count++;
			}
		}
		return (count > 0) ? avgskill / count : AISkillSetting::DefaultSkill;
	}
}

void Activity::ReassignSquadLeader(const int player, const int team) {
	if (m_ControlledActor[player]->GetAIMode() == Actor::AIMODE_SQUAD) {
		MOID leaderID = m_ControlledActor[player]->GetAIMOWaypointID();

		if (leaderID != g_NoMOID) {
			Actor* actor = g_MovableMan.GetNextTeamActor(team, m_ControlledActor[player]);

			do {
				// Set the controlled actor as new leader if actor follow the old leader, and not player controlled and not brain
				if (actor && (actor->GetAIMode() == Actor::AIMODE_SQUAD) && (actor->GetAIMOWaypointID() == leaderID) && !actor->GetController()->IsPlayerControlled() && !actor->IsInGroup("Brains")) {
					actor->ClearAIWaypoints();
					actor->AddAIMOWaypoint(m_ControlledActor[player]);
					// Make sure actor has m_ControlledActor registered as an AIMOWaypoint
					actor->SetMovePathToUpdate();
				} else if (actor && actor->GetID() == leaderID) {
					// Set the old leader to follow the controlled actor and inherit his AI mode
					m_ControlledActor[player]->ClearAIWaypoints();
					m_ControlledActor[player]->SetAIMode(static_cast<Actor::AIMode>(actor->GetAIMode()));

					if (m_ControlledActor[player]->GetAIMode() == Actor::AIMODE_GOTO) {
						// Copy the old leaders move orders
						if (actor->GetAIMOWaypointID() != g_NoMOID) {
							const MovableObject* targetMO = g_MovableMan.GetMOFromID(actor->GetAIMOWaypointID());
							if (targetMO) {
								m_ControlledActor[player]->AddAIMOWaypoint(targetMO);
							}
						} else if ((actor->GetLastAIWaypoint() - actor->GetPos()).GetLargest() > 1) {
							m_ControlledActor[player]->AddAISceneWaypoint(actor->GetLastAIWaypoint());
						}
					}
					actor->ClearAIWaypoints();
					actor->SetAIMode(Actor::AIMODE_SQUAD);
					actor->AddAIMOWaypoint(m_ControlledActor[player]);
					actor->SetMovePathToUpdate();
				}
				actor = g_MovableMan.GetNextTeamActor(team, actor);
			} while (actor && actor != m_ControlledActor[player]);
		}
	}
}

int Activity::GetLockstepHumanSlotIndex(int team) const {
	return ScenarioRunner::GetLockstepHumanSlotIndex(team);
}

bool Activity::SwitchToActor(Actor* actor, int player, int team) {
	if (team < Teams::TeamOne || team >= Teams::MaxTeamCount || player < Players::PlayerOne || player >= Players::MaxPlayerCount || !m_IsHuman[player]) {
		return false;
	}
	if (!actor || !g_MovableMan.IsActor(actor) || !actor->IsPlayerControllable()) {
		return false;
	}
	if ((actor != m_Brain[player] && actor->GetController()->IsSeatedByPlayer()) || IsOtherPlayerBrain(actor, player)) {
		g_GUISound.UserErrorSound()->Play(player);
		return false;
	}

	Actor* preSwitchActor = (m_ControlledActor[player] && g_MovableMan.IsActor(m_ControlledActor[player])) ? m_ControlledActor[player] : nullptr;
	if (preSwitchActor && preSwitchActor->GetController()->IsSeatedByPlayer(player)) {
		preSwitchActor->SetControllerMode(Controller::CIM_AI);
	}

	m_ControlledActor[player] = actor;
	if (m_ControlledActor[player]->GetTeam() != team) {
		m_ControlledActor[player]->SetTeam(team);
	}
	m_ControlledActor[player]->SetControllerMode(Controller::CIM_PLAYER, player);

	SoundContainer* actorSwitchSoundToPlay = (m_ControlledActor[player] == m_Brain[player]) ? g_GUISound.BrainSwitchSound() : g_GUISound.ActorSwitchSound();
	// Snapshot Start selects the brain again; the UI click is not part of the restored sim.
	if (!g_MovableMan.IsRestoringSnapshot()) {
		actorSwitchSoundToPlay->Play(player);
	}

	// If out of frame from the POV of the preswitch actor, play the camera travel noise
	const int switchSoundThreshold = g_WindowMan.GetResX() / 2;
	if (!g_MovableMan.IsRestoringSnapshot() && preSwitchActor && g_SceneMan.ShortestDistance(preSwitchActor->GetPos(), m_ControlledActor[player]->GetPos(), g_SceneMan.SceneWrapsX() || g_SceneMan.SceneWrapsY()).MagnitudeIsGreaterThan(static_cast<float>(switchSoundThreshold))) {
		g_GUISound.CameraTravelSound()->Play(player);
	}

	ReassignSquadLeader(player, team);
	m_ViewState[player] = Normal;

	// Taking control of a teammate's actor in a lockstep match moves its frame production here;
	// the handoff crosses the wire so every peer flips the actor's owner at the same frame.
	if (ScenarioRunner::IsLockstepControllerSyncActive()) {
		const uint8_t localPeerId = ScenarioRunner::GetLockstepLocalPeerId();
		const int64_t actorUID = static_cast<int64_t>(m_ControlledActor[player]->GetUniqueID());
		if (ScenarioRunner::GetLockstepActorOwner(actorUID, team, !m_ControlledActor[player]->IsPlayerControlled()) != localPeerId) {
			ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{localPeerId, NetGameSwitchControl{actorUID, team, localPeerId}});
		}
		// The actor left behind goes back to the owner the world seeded it with, so its AI runs there.
		if (preSwitchActor) {
			const int64_t preSwitchUID = static_cast<int64_t>(preSwitchActor->GetUniqueID());
			const int preSwitchTeam = preSwitchActor->GetTeam();
			const uint8_t seededOwner = NetActorOwnership::GetSeededOwner(preSwitchUID);
			if (seededOwner != 0 && seededOwner != localPeerId && ScenarioRunner::GetLockstepActorOwner(preSwitchUID, preSwitchTeam, !preSwitchActor->IsPlayerControlled()) == localPeerId) {
				ScenarioRunner::EnqueueLocalGameCommand(NetGameCommand{localPeerId, NetGameSwitchControl{preSwitchUID, preSwitchTeam, seededOwner}});
			}
		}
	}

	return true;
}

void Activity::ReleaseLockstepControlOfActor(int player) {
	if (player < Players::PlayerOne || player >= Players::MaxPlayerCount) {
		return;
	}
	if (Actor* actor = m_ControlledActor[player]; actor && g_MovableMan.IsActor(actor)) {
		actor->SetControllerMode(Controller::CIM_AI);
	}
	m_ControlledActor[player] = nullptr;
	m_ViewState[player] = ViewState::DeathWatch;
	m_DeathTimer[player].Reset();
}

void Activity::LoseControlOfActor(int player) {
	if (player >= Players::PlayerOne && player < Players::MaxPlayerCount) {
		if (Actor* actor = m_ControlledActor[player]; actor && g_MovableMan.IsActor(actor)) {
			actor->SetControllerMode(Controller::CIM_AI);
			actor->GetController()->SetDisabled(false);
		}
		m_ControlledActor[player] = nullptr;
		m_ViewState[player] = ViewState::DeathWatch;
		m_DeathTimer[player].Reset();
	}
}

void Activity::HandleCraftEnteringOrbit(ACraft* orbitedCraft) {
	if (!orbitedCraft) {
		return;
	}

	char messageString[64];
	float foreignCostMult = 0.9F;
	float nativeCostMult = 0.9F;
	int orbitedCraftTeam = orbitedCraft->GetTeam();
	bool brainOnBoard = orbitedCraft->HasObjectInGroup("Brains");

	if (g_MetaMan.GameInProgress()) {
		for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; player++) {
			if (GetTeamOfPlayer(static_cast<Players>(player)) == orbitedCraftTeam) {
				const MetaPlayer* metaPlayer = g_MetaMan.GetMetaPlayerOfInGamePlayer(player);
				if (metaPlayer) {
					foreignCostMult = metaPlayer->GetForeignCostMultiplier();
					nativeCostMult = metaPlayer->GetNativeCostMultiplier();
				}
			}
		}
	}

	float totalValue = orbitedCraft->GetTotalValue(0, foreignCostMult, nativeCostMult);

	std::string craftText = "Returned craft";
	if (!orbitedCraft->IsInventoryEmpty()) {
		craftText += " + cargo";
	}
	if (totalValue > 0.0F) {
		m_TeamFunds[orbitedCraftTeam] += totalValue;
		std::snprintf(messageString, sizeof(messageString), "%s added %.0f oz to funds!", craftText.c_str(), totalValue);
	}
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (m_IsActive[player]) {
			if (brainOnBoard && orbitedCraft == GetPlayerBrain(static_cast<Players>(player))) {
				m_BrainEvacuated[player] = true;
				g_FrameMan.ClearScreenText(ScreenOfPlayer(static_cast<Players>(player)));
				g_FrameMan.SetScreenText("YOUR BRAIN HAS BEEN EVACUATED BACK INTO ORBIT!", ScreenOfPlayer(static_cast<Players>(player)), 0, 3500);
			} else if (m_Team[player] == orbitedCraftTeam && totalValue > 0.0F) {
				g_FrameMan.ClearScreenText(ScreenOfPlayer(static_cast<Players>(player)));
				g_FrameMan.SetScreenText(messageString, ScreenOfPlayer(static_cast<Players>(player)), 0, 3500);
				m_MessageTimer[player].Reset();
			}
		}
	}

	orbitedCraft->SetGoldCarried(0);
	orbitedCraft->SetHealth(orbitedCraft->GetMaxHealth());

	// The craft entering orbit will count as a death for the team because it's being deleted, so we need to decrement the team's death count to keep it correct.
	m_TeamDeaths[orbitedCraftTeam]--;
}

int Activity::GetBrainCount(bool getForHuman) const {
	int brainCount = 0;

	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (getForHuman) {
			if (m_IsActive[player] && m_IsHuman[player] && m_HadBrain[player] && g_MovableMan.IsActor(m_Brain[player]) && m_Brain[player]->HasObjectInGroup("Brains")) {
				brainCount++;
			}
		} else {
			if (m_IsActive[player] && !m_IsHuman[player] && m_HadBrain[player] && g_MovableMan.IsActor(m_Brain[player]) && m_Brain[player]->HasObjectInGroup("Brains")) {
				brainCount++;
			}
		}
	}
	return brainCount;
}

void Activity::SwitchToPrevOrNextActor(bool nextActor, int player, int team, const Actor* actorToSkip) {
	if (team < Teams::TeamOne || team >= Teams::MaxTeamCount || player < Players::PlayerOne || player >= Players::MaxPlayerCount || !m_IsHuman[player]) {
		return;
	}

	Actor* preSwitchActor = m_ControlledActor[player];
	Actor* actorToSwitchTo = nextActor ? g_MovableMan.GetNextTeamActor(team, preSwitchActor) : g_MovableMan.GetPrevTeamActor(team, preSwitchActor);
	const Actor* firstAttemptedSwitchActor = nullptr;

	bool actorSwitchSucceedOrTriedAllActors = false;
	while (!actorSwitchSucceedOrTriedAllActors) {
		if (actorToSwitchTo == preSwitchActor) {
			g_GUISound.UserErrorSound()->Play(player);
			actorSwitchSucceedOrTriedAllActors = true;
		} else if (actorToSwitchTo == firstAttemptedSwitchActor) {
			if (m_Brain[player]) {
				SwitchToActor(m_Brain[player]);
			} else {
				m_ControlledActor[player] = nullptr;
			}
			actorSwitchSucceedOrTriedAllActors = true;
		} else {
			actorSwitchSucceedOrTriedAllActors = (actorToSwitchTo != actorToSkip && SwitchToActor(actorToSwitchTo, player, team));
		}
		firstAttemptedSwitchActor = firstAttemptedSwitchActor ? firstAttemptedSwitchActor : actorToSwitchTo;
		if (!actorSwitchSucceedOrTriedAllActors) {
			actorToSwitchTo = nextActor ? g_MovableMan.GetNextTeamActor(team, actorToSwitchTo) : g_MovableMan.GetPrevTeamActor(team, actorToSwitchTo);
		}
	}
}

void Activity::Update() {
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (m_MessageTimer[player].IsPastSimMS(5000)) {
			g_FrameMan.ClearScreenText(ScreenOfPlayer(player));
		}
		if (m_IsActive[player]) {
			m_PlayerController[player].Update();
		}
	}
}

void Activity::RenderUpdate() {
	// Keep player controllers' analog cursor tracking latest mouse/stick each render frame
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		if (m_IsActive[player]) {
			m_PlayerController[player].RenderUpdate();
		}
	}
}

bool Activity::CanBeUserSaved() const {
	if (const Scene* scene = g_SceneMan.GetScene(); (scene && scene->IsMetagameInternal()) || g_MetaMan.GameInProgress()) {
		return false;
	}

	return m_AllowsUserSaving;
}

void Activity::CaptureRollbackState(RollbackState& out) const {
	out.state = m_ActivityState;
	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		out.teamFunds[team] = m_TeamFunds[team];
		out.teamDeaths[team] = m_TeamDeaths[team];
		out.teamActive[team] = m_TeamActive[team];
	}
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		out.controlledActorUID[player] = m_ControlledActor[player] ? m_ControlledActor[player]->GetUniqueID() : 0;
		out.brainUID[player] = m_Brain[player] ? m_Brain[player]->GetUniqueID() : 0;
		out.brainEvacuated[player] = m_BrainEvacuated[player];
	}
}

void Activity::RestoreRollbackState(const RollbackState& in) {
	m_ActivityState = in.state;
	for (int team = Teams::TeamOne; team < Teams::MaxTeamCount; ++team) {
		m_TeamFunds[team] = in.teamFunds[team];
		m_TeamDeaths[team] = in.teamDeaths[team];
		m_TeamActive[team] = in.teamActive[team];
	}
	// The slots name their actors by unique id, so they land on whichever copy of the actor the world holds now.
	for (int player = Players::PlayerOne; player < Players::MaxPlayerCount; ++player) {
		m_ControlledActor[player] = in.controlledActorUID[player] ? dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(in.controlledActorUID[player])) : nullptr;
		m_Brain[player] = in.brainUID[player] ? dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(in.brainUID[player])) : nullptr;
		m_BrainEvacuated[player] = in.brainEvacuated[player];
	}
}

int64_t Activity::NetActorUID(const Actor* actor) {
	return actor && g_MovableMan.IsKnownObject(actor) ? static_cast<int64_t>(actor->GetUniqueID()) : 0;
}

Actor* Activity::ResolveNetActor(int64_t uid) {
	if (uid <= 0 || uid > std::numeric_limits<long>::max()) return nullptr;
	auto* actor = dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(static_cast<long>(uid)));
	return actor && !actor->IsDead() ? actor : nullptr;
}

void Activity::CaptureNetPlayerBindings(NetGamePlayerBindings& out) const {
	out = {};
	for (int player = 0; player < Players::MaxPlayerCount; ++player) {
		auto& binding = out.players[player];
		binding.active = m_IsActive[player];
		binding.human = m_IsHuman[player];
		binding.hadBrain = m_HadBrain[player];
		binding.brainEvacuated = m_BrainEvacuated[player];
		binding.team = static_cast<int8_t>(m_Team[player]);
		binding.viewState = static_cast<uint8_t>(m_ViewState[player]);
		binding.controlledUID = NetActorUID(m_ControlledActor[player]);
		binding.brainUID = NetActorUID(m_Brain[player]);
		const Vector camera = g_CameraMan.GetScrollTarget(m_PlayerScreen[player]);
		binding.cameraX = camera.m_X;
		binding.cameraY = camera.m_Y;
	}
}

bool Activity::CaptureNetLocalPlayerState(NetLocalPlayerState& out) const {
	try {
		NetLocalPlayerState state;
		CaptureNetPlayerBindings(state.bindings);
		state.playerCount = m_PlayerCount;
		for (int player = 0; player < Players::MaxPlayerCount; ++player) {
			state.screens[player] = m_PlayerScreen[player];
			state.fundsShare[player] = m_TeamFundsShare[player];
			state.fundsContribution[player] = m_FundsContribution[player];
			state.deathTimers[player] = m_DeathTimer[player];
			state.messageTimers[player] = m_MessageTimer[player];
			state.controllers[player] = m_PlayerController[player].SaveCheckpoint();
			state.controllerActorUIDs[player] = NetActorUID(m_PlayerController[player].GetControlledActor());
		}
		for (MovableObject* object: g_MovableMan.SnapshotKnownObjects()) {
			if (auto* actor = dynamic_cast<Actor*>(object)) {
				state.actorInputs.emplace_back(actor->GetUniqueID(), actor->GetController()->CaptureLocalInputState());
			}
		}
		state.camera = g_CameraMan.SaveCheckpoint();
		out = std::move(state);
		return true;
	} catch (const std::exception&) { return false; }
}

bool Activity::ApplyNetPlayerSlots(const NetGamePlayerBindings& bindings) {
	for (const auto& player: bindings.players) {
		if (player.team < Teams::NoTeam || player.team >= Teams::MaxTeamCount || player.viewState > ViewState::UnitSelectCircle ||
			player.controlledUID < 0 || player.brainUID < 0 || !std::isfinite(player.cameraX) || !std::isfinite(player.cameraY) ||
			std::any_of(player.viewTargets.begin(), player.viewTargets.end(), [](float value) { return !std::isfinite(value); })) return false;
	}
	m_PlayerCount = 0;
	int screen = 0;
	for (int player = 0; player < Players::MaxPlayerCount; ++player) {
		const auto& binding = bindings.players[player];
		m_IsActive[player] = binding.active;
		m_IsHuman[player] = binding.human;
		m_HadBrain[player] = binding.hadBrain;
		m_BrainEvacuated[player] = binding.brainEvacuated;
		m_Team[player] = binding.team;
		m_ViewState[player] = static_cast<ViewState>(binding.viewState);
		m_Brain[player] = ResolveNetActor(binding.brainUID);
		m_ControlledActor[player] = ResolveNetActor(binding.controlledUID);
		m_PlayerCount += binding.active ? 1 : 0;
		m_PlayerScreen[player] = binding.active && binding.human ? screen++ : -1;
	}
	m_HasCheckpointActorIDs = false;
	return true;
}

bool Activity::RestoreNetLocalPlayerState(const NetLocalPlayerState& state) {
	if (state.playerCount < 0 || state.playerCount > Players::MaxPlayerCount || !g_CameraMan.LoadCheckpoint(state.camera, true)) return false;
	for (int player = 0; player < Players::MaxPlayerCount; ++player) {
		if (state.screens[player] < -1 || state.screens[player] >= Players::MaxPlayerCount ||
			!m_PlayerController[player].LoadCheckpoint(state.controllers[player], true)) return false;
	}
	std::map<int64_t, const Controller::LocalInputState*> inputs;
	for (const auto& [uid, input]: state.actorInputs) {
		if (uid <= 0 || !inputs.emplace(uid, &input).second) return false;
	}
	if (!ApplyNetPlayerSlots(state.bindings)) return false;
	m_PlayerCount = state.playerCount;
	for (int player = 0; player < Players::MaxPlayerCount; ++player) {
		m_PlayerScreen[player] = state.screens[player];
		m_TeamFundsShare[player] = state.fundsShare[player];
		m_FundsContribution[player] = state.fundsContribution[player];
		m_DeathTimer[player] = state.deathTimers[player];
		m_MessageTimer[player] = state.messageTimers[player];
		if (!m_PlayerController[player].LoadCheckpoint(state.controllers[player])) return false;
		m_PlayerController[player].SetControlledActor(ResolveNetActor(state.controllerActorUIDs[player]));
	}
	for (MovableObject* object: g_MovableMan.SnapshotKnownObjects()) {
		if (auto* actor = dynamic_cast<Actor*>(object)) {
			const auto found = inputs.find(actor->GetUniqueID());
			if (found == inputs.end()) actor->GetController()->ResetLocalInputState();
			else actor->GetController()->RestoreLocalInputState(*found->second);
		}
	}
	return g_CameraMan.LoadCheckpoint(state.camera);
}

bool Activity::ApplyNetPlayerBindings(const NetGamePlayerBindings& bindings) {
	if (!ApplyNetPlayerSlots(bindings)) return false;
	for (MovableObject* object: g_MovableMan.SnapshotKnownObjects()) {
		if (auto* actor = dynamic_cast<Actor*>(object)) actor->GetController()->ResetLocalInputState();
	}
	for (int player = 0; player < Players::MaxPlayerCount; ++player) {
		m_PlayerController[player].Reset();
		m_PlayerController[player].Create(Controller::CIM_PLAYER, player);
		m_PlayerController[player].SetTeam(m_Team[player]);
		m_DeathTimer[player].Reset();
		m_MessageTimer[player].Reset();
		m_TeamFundsShare[player] = 0;
		m_FundsContribution[player] = 0;
		if (m_IsActive[player] && m_IsHuman[player] && m_ControlledActor[player]) {
			m_ControlledActor[player]->GetController()->ResetLocalInputState(Controller::CIM_PLAYER, player);
		}
		if (m_PlayerScreen[player] >= 0) {
			const auto& binding = bindings.players[player];
			g_CameraMan.SetScreenTeam(m_Team[player], m_PlayerScreen[player]);
			g_CameraMan.SetScreenOcclusion(Vector(), m_PlayerScreen[player]);
			g_CameraMan.SetScrollTarget(Vector(binding.cameraX, binding.cameraY), 1.0F, m_PlayerScreen[player]);
			g_CameraMan.SetScroll(Vector(binding.cameraX, binding.cameraY), m_PlayerScreen[player]);
		}
	}
	return true;
}

bool Activity::RunNetLocalPlayerStateSelfTest() {
	bool passed = true;
	const auto check = [&](const char* name, bool value) {
		passed = passed && value;
		std::cout << "[net-local-state-selftest] " << (value ? "PASS" : "FAIL") << " " << name << std::endl;
	};
	struct Restore {
		long uid = MovableObject::GetUniqueIDCounter();
		std::string camera = g_CameraMan.SaveCheckpoint();
		std::vector<std::pair<Actor*, Controller::LocalInputState>> inputs;
		Restore() { for (auto* object: g_MovableMan.SnapshotKnownObjects()) if (auto* actor = dynamic_cast<Actor*>(object)) inputs.emplace_back(actor, actor->GetController()->CaptureLocalInputState()); }
		~Restore() { for (const auto& [actor, input]: inputs) actor->GetController()->RestoreLocalInputState(input); g_CameraMan.LoadCheckpoint(camera); MovableObject::PinUniqueIDCounter(uid); }
	} restore;
	try {
		GameActivity fixture;
		Actor carrier, controlled;
		auto brain = std::make_unique<Actor>();
		if (carrier.MovableObject::Create() < 0 || controlled.MovableObject::Create() < 0 || brain->MovableObject::Create() < 0) throw std::runtime_error("local binding actors could not be created");
		Actor* exactBrain = brain.get();
		exactBrain->SetTeam(TeamFour);
		carrier.AddInventoryItem(brain.release());
		fixture.m_Brain[0] = exactBrain;
		fixture.m_ControlledActor[0] = &controlled;
		fixture.m_Team[0] = TeamTwo;
		fixture.m_PlayerController[0].Create(Controller::CIM_PLAYER, 0);
		fixture.m_PlayerController[0].SetControlledActor(&controlled);
		controlled.GetController()->ResetLocalInputState(Controller::CIM_PLAYER, 0);
		NetLocalPlayerState local;
		check("capture_exact_inventory_brain", fixture.Activity::CaptureNetLocalPlayerState(local) && local.bindings.players[0].brainUID == exactBrain->GetUniqueID());
		fixture.m_TeamFunds[0] = 739;
		fixture.m_TeamDeaths[0] = 17;
		fixture.m_ActivityState = ActivityState::Over;
		fixture.m_Difficulty = 83;
		std::array<bool, ControlState::CONTROLSTATECOUNT> bits{};
		bits[ControlState::WEAPON_FIRE] = true;
		Controller& controller = *controlled.GetController();
		controller.ApplyWireState(bits, Vector(0.25F, -0.5F), Vector(-0.75F, 0.625F), Vector(11, 13), Vector(17, 19), Controller::CIM_NETWORK, 3, true);
		controller.ApplyWireScheme(Controller::WireDeviceClass::Gamepad, 1.75F);
		controller.SetWireApplyTick(137);
		const auto canonical = [&] {
			CheckpointWriter writer("NetControllerAuthority");
			writer(controller.GetInputMode(), controller.GetPlayerRaw(), controller.IsQuickDisabled(), controller.GetWireApplyTick(), controller.HasWireScheme(),
				controller.GetWireDeviceClass(), controller.GetWireDigitalAimSpeed(), controller.GetAnalogMove(), controller.GetAnalogAim(), controller.GetAnalogCursor(), controller.GetMouseMovement());
			for (int state = 0; state < ControlState::CONTROLSTATECOUNT; ++state) writer(controller.IsState(static_cast<ControlState>(state)));
			return writer.Text();
		};
		const std::string authority = canonical();
		fixture.m_Brain[0] = nullptr;
		fixture.m_ControlledActor[0] = nullptr;
		check("restore_inventory_brain_without_team_write", fixture.Activity::RestoreNetLocalPlayerState(local) && fixture.m_Brain[0] == exactBrain && exactBrain->GetTeam() == TeamFour);
		check("preserve_shared_activity_authority", fixture.m_TeamFunds[0] == 739 && fixture.m_TeamDeaths[0] == 17 && fixture.m_ActivityState == ActivityState::Over && fixture.m_Difficulty == 83);
		check("preserve_wire_state_and_restore_local_seat", canonical() == authority && controller.GetSeatMode() == Controller::CIM_PLAYER && controller.GetSeatPlayerRaw() == 0);
		check("restore_exact_control_alias", fixture.m_ControlledActor[0] == &controlled && fixture.m_PlayerController[0].GetControlledActor() == &controlled);
		exactBrain->SetStatus(Actor::DEAD);
		check("dead_brain_clears_without_substitution", fixture.Activity::RestoreNetLocalPlayerState(local) && !fixture.m_Brain[0] && canonical() == authority);
		NetGamePlayerBindings fresh = local.bindings;
		for (auto& binding: fresh.players) binding.active = false;
		fresh.players[0].brainUID = std::numeric_limits<int64_t>::max();
		check("fresh_missing_identity_is_null", fixture.Activity::ApplyNetPlayerBindings(fresh) && !fixture.m_Brain[0] && fixture.m_ControlledActor[0] == &controlled && canonical() == authority);
	} catch (const std::exception& exception) { check(exception.what(), false); }
	return passed;
}

std::string Activity::SaveCheckpoint() const {
	CheckpointWriter writer("Activity3");
	VisitCheckpoint(writer, *this);
	std::array<std::array<long, 3>, Players::MaxPlayerCount> links{};
	for (int player = 0; player < Players::MaxPlayerCount; ++player) {
		if (m_HasCheckpointActorIDs) { links[player] = m_CheckpointActorIDs[player]; continue; }
		links[player] = {m_Brain[player] ? m_Brain[player]->GetUniqueID() : 0,
			m_ControlledActor[player] ? m_ControlledActor[player]->GetUniqueID() : 0,
			m_PlayerController[player].GetControlledActor() ? m_PlayerController[player].GetControlledActor()->GetUniqueID() : 0};
	}
	writer(links, Icon::SaveCheckpointSet(m_TeamIcons));
	return writer.Text();
}

bool Activity::LoadCheckpoint(std::string_view text, bool validateOnly) {
	try {
		const bool legacy = text.starts_with("9 Activity1 ");
		CheckpointReader reader(text, legacy ? "Activity1" : "Activity3", validateOnly);
		VisitCheckpoint(reader, *this);
		reader(m_CheckpointActorIDs);
		if (!legacy) {
			std::string icons;
			reader.Value(icons);
			auto apply = Icon::PrepareCheckpointSet(icons, m_TeamIcons, validateOnly);
			if (apply) reader.OnCommit(std::move(apply));
		}
		reader.OnCommit([this] { m_HasCheckpointActorIDs = true; });
		reader.Finish();
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool Activity::ApplyPendingCheckpoint() {
	if (m_PendingRuntimeCheckpoint.empty()) return true;
	if (!LoadCheckpoint(m_PendingRuntimeCheckpoint)) return false;
	m_PendingRuntimeCheckpoint.clear();
	return true;
}

bool Activity::ResolveCheckpointReferences() {
	if (!m_HasCheckpointActorIDs) return true;
	std::array<std::array<Actor*, 3>, Players::MaxPlayerCount> actors{};
	for (int player = 0; player < Players::MaxPlayerCount; ++player) {
		for (int link = 0; link < 3; ++link) {
			const long uid = m_CheckpointActorIDs[player][link];
			actors[player][link] = uid ? dynamic_cast<Actor*>(g_MovableMan.FindObjectByUniqueID(uid)) : nullptr;
			if (uid && !actors[player][link]) return false;
		}
	}
	for (int player = 0; player < Players::MaxPlayerCount; ++player) {
		m_Brain[player] = actors[player][0];
		m_ControlledActor[player] = actors[player][1];
		m_PlayerController[player].SetControlledActor(actors[player][2]);
	}
	m_HasCheckpointActorIDs = false;
	return true;
}
