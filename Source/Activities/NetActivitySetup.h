#pragma once

#include <string>

namespace RTE {

	class Activity;
	class GameActivity;
	struct NetMatchConfig;
	struct NetMatchStandardRules;

	/// The one place a networked match turns its agreed config into a configured Activity, so every peer,
	/// the dedicated host, the command line end-to-end path and a replay launch the identical descriptor.
	class NetActivitySetup {
	public:
		/// Clones the config's module qualified activity, stages its site and applies the agreed rules the
		/// way the Scenario setup does. Null with the reason every peer refuses on, so a missing activity,
		/// scene or technology module fails the launch instead of quietly launching something else.
		/// @param localTeam The peer's own team, or Activity::NoTeam for a dedicated host that seats nobody.
		/// @return The configured activity, ownership transferred to the caller.
		static Activity* CreateConfiguredActivity(const NetMatchConfig& config, int localTeam, std::string* error);

		/// Applies the agreed standard rules to an activity before it starts, mirroring the setter order of
		/// ScenarioActivityConfigGUI::StartGame. False with a reason when a rule names something uninstalled.
		static bool ApplyStandardRules(const NetMatchStandardRules& rules, GameActivity& activity, std::string* error);
	};

} // namespace RTE
