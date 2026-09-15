#include "NetActivitySetup.h"

#include "Activity.h"
#include "GameActivity.h"
#include "NetMatchConfig.h"
#include "PresetMan.h"
#include "Scene.h"
#include "SceneMan.h"
#include "ScenarioRunner.h"

namespace RTE {

	namespace {
		bool Refuse(std::string* error, const std::string& reason) {
			if (error) {
				*error = reason;
			}
			return false;
		}

		/// The preset the named module defines, or null. GetEntityPreset falls back to the official modules
		/// when its own module has no such preset; the peers agreed on one module, so hold the lookup to it.
		const Entity* FindInModule(const std::string& type, const std::string& module, const std::string& preset) {
			const int moduleId = g_PresetMan.GetModuleID(module);
			if (moduleId < 0) {
				return nullptr;
			}
			const Entity* found = g_PresetMan.GetEntityPreset(type, preset, moduleId);
			return (found && found->GetModuleID() == moduleId) ? found : nullptr;
		}
	} // namespace

	bool NetActivitySetup::ApplyStandardRules(const NetMatchStandardRules& rules, GameActivity& activity, std::string* error) {
		activity.SetDifficulty(rules.difficulty);
		// The top of the original gold slider means the infinite value; the config carries that same sentinel.
		activity.SetStartingGold(static_cast<int>(rules.startingGold));
		activity.SetRequireClearPathToOrbit(rules.requireClearPathToOrbit);
		activity.SetFogOfWarEnabled(rules.fogOfWar);
		for (int team = Activity::Teams::TeamOne; team < Activity::Teams::MaxTeamCount; ++team) {
			const NetMatchTeamRules& teamRules = rules.teamRules[team];
			// The host resolved -Random- into a module before the config shipped; no module means -All-.
			const std::string tech = teamRules.technologyModule.empty() ? "-All-" : teamRules.technologyModule;
			if (tech != "-All-" && g_PresetMan.GetModuleID(tech) < 0) {
				return Refuse(error, "team " + std::to_string(team) + " technology " + tech + " is not installed");
			}
			activity.SetTeamTech(team, tech);
			activity.SetTeamAISkill(team, teamRules.aiSkill);
		}
		return true;
	}

	Activity* NetActivitySetup::CreateConfiguredActivity(const NetMatchConfig& config, int localTeam, std::string* error) {
		// A dedicated host owns no seat: NoTeam means clear the players but still run every roster team.
		if (localTeam != Activity::Teams::NoTeam && (localTeam < Activity::Teams::TeamOne || localTeam >= Activity::Teams::MaxTeamCount)) {
			Refuse(error, "invalid local team");
			return nullptr;
		}
		const Activity* presetActivity = dynamic_cast<const Activity*>(FindInModule(config.activityType, config.activityModule, config.activityPreset));
		if (!presetActivity) {
			Refuse(error, "match activity " + config.activityModule + "/" + config.activityPreset + " is not installed");
			return nullptr;
		}
		// The agreed site. A config from before the rules existed carries no site and keeps launching the
		// activity's own scene. Resolved here but staged last, so a refused launch leaves nothing behind.
		const bool ownSite = config.sceneName.empty();
		const std::string sceneName = ownSite ? presetActivity->GetSceneName() : config.sceneName;
		const Scene* scene = nullptr;
		if (!sceneName.empty()) {
			scene = dynamic_cast<const Scene*>(ownSite ? g_PresetMan.GetEntityPreset("Scene", sceneName) : FindInModule("Scene", config.sceneModule, sceneName));
			if (!scene) {
				Refuse(error, "match scene " + (ownSite ? sceneName : config.sceneModule + "/" + sceneName) + " is not installed");
				return nullptr;
			}
		}
		Activity* activity = dynamic_cast<Activity*>(presetActivity->Clone());
		if (!activity) {
			Refuse(error, "could not create the match activity");
			return nullptr;
		}
		if (GameActivity* gameActivity = dynamic_cast<GameActivity*>(activity)) {
			std::string ruleError;
			if (!ApplyStandardRules(config, *gameActivity, &ruleError)) {
				delete activity;
				Refuse(error, ruleError);
				return nullptr;
			}
			gameActivity->ClearPlayers(false);
			if (!gameActivity->ConfigureLockstepPlayers() && localTeam != Activity::Teams::NoTeam) {
				gameActivity->AddPlayer(Players::PlayerOne, true, localTeam, 0);
			}
			// Activate every team in the synced roster so all peers run the identical team set, and seat each
			// on the agreed gold. The activity's own script still has the last word through GetStartingGold.
			for (int team = Activity::Teams::TeamOne; team < Activity::Teams::MaxTeamCount; ++team) {
				if (team == localTeam || ScenarioRunner::IsLockstepActiveTeam(team)) {
					gameActivity->ForceSetTeamAsActive(team);
					gameActivity->SetTeamFunds(static_cast<float>(gameActivity->GetStartingGold()), team);
				}
			}
		}
		// Staged the way the Scenario setup stages its selected scene, with the agreed deploy-units choice.
		if (scene) {
			g_SceneMan.SetSceneToLoad(scene, true, config.deployUnits);
		}
		return activity;
	}

} // namespace RTE
