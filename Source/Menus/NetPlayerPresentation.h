#pragma once

#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "ScenarioRunner.h"

#include <map>
#include <string>

namespace RTE::NetPlayerPresentation {

	inline uint64_t session = 0;
	inline std::map<uint8_t, std::string> names;

	inline bool Placeholder(uint8_t peer, const std::string& name) {
		return name.empty() || name == "Client " + std::to_string(peer) || name == "Player " + std::to_string(peer);
	}

	inline void Remember(const NetLobbySnapshot& snapshot, const std::string& localName) {
		const uint64_t current = g_NetMatchService.GetLobbyMatchConfig().sessionId;
		if ((current && current != session) || snapshot.serviceState == "Idle") names.clear();
		if (current) session = current;
		for (const auto& member: snapshot.members) {
			std::string name = member.displayName;
			if ((member.isLocal || member.peerId == snapshot.localPeerId) && Placeholder(member.peerId, name) && !localName.empty()) name = localName;
			if (!Placeholder(member.peerId, name)) names[member.peerId] = name;
		}
	}

	inline std::string Name(uint8_t peer, const std::string& fallback) {
		if (Placeholder(peer, fallback)) {
			if (const auto known = names.find(peer); known != names.end()) return known->second;
		}
		return fallback.empty() ? "Player " + std::to_string(peer) : fallback;
	}

	inline std::string Name(const NetLobbyMember& member) { return Name(member.peerId, member.displayName); }

	inline std::string State(uint8_t peer, bool aiHeld, bool dropped, bool reclaiming) {
		const auto presence = g_NetMatchService.GetSeatPresence().StateOf(peer);
		const bool left = presence == NetSeatPresenceState::Left;
		const bool ai = aiHeld || ScenarioRunner::IsLockstepSeatUnderAI(peer, ScenarioRunner::GetLockstepCompletedFrame());
		if (left) return ai ? "Left - AI in control" : "Left";
		if (ai) return reclaiming ? "Held - AI in control - rejoining" : "Held - AI in control";
		if (reclaiming) return "Rejoining";
		if (dropped) return "Disconnected";
		return "Connected";
	}

	inline std::string State(const NetLobbyMember& member) {
		return State(member.peerId, member.aiHeld, member.dropped, member.reclaiming);
	}

	inline std::string Row(const NetLobbyMember& member) {
		std::string row = Name(member) + "  /  " + State(member);
		if (!member.connectedRoute.empty()) row += " / via " + member.connectedRoute;
		return row;
	}
}
