#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace RTE {

	class Actor;

	/// A scripted stand-in for a mod's AI writing to its actor directly: at a tick, inside the owner's AI
	/// pass, it makes an equip call, flips the facing or writes the aim of a local AI actor, so the
	/// controller-boundary fixtures exercise exactly the writes the wire must carry.
	///
	/// Script lines: `<tick> team=<n> <op> [args]`, targeting the lowest-id local AI-driven AHuman of the team.
	/// Ops: equip-firearm · equip-group <group> · equip-loaded <group> <exclude> · equip-named <preset> ·
	/// equip-throwable · equip-digger · equip-shield · equip-shield-bg · unequip-fg · unequip-bg · flip <0|1> · aim <radians> ·
	/// scene-waypoint <x> <y> · clear-waypoints.
	class AIWriteScript {
	public:
		static bool Load(const std::string& path, std::string* error = nullptr);
		static bool IsActive() { return s_Active; }

		/// Runs the lines due at the tick; called by the AI pass for the actors this machine drives.
		static void RunTick(uint64_t simTick, const std::deque<Actor*>& actors, const std::function<bool(const Actor*)>& isLocal);

	private:
		static bool s_Active;
	};
} // namespace RTE
