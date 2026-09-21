#pragma once

#include "NetLobbySnapshot.h"
#include "NetMatchService.h"
#include "ScenarioRunner.h"

#include <map>
#include <set>
#include <string>

namespace RTE::NetPlayerPresentation {

	inline uint64_t session = 0;
	inline std::map<uint8_t, std::string> names;
	// A seat only leaves a session it was seen in: a peer still connecting has no departure to show.
	inline std::set<uint8_t> seated;

	inline bool Placeholder(uint8_t peer, const std::string& name) {
		return name.empty() || name == "Client " + std::to_string(peer) || name == "Player " + std::to_string(peer);
	}

	inline void Remember(const NetLobbySnapshot& snapshot, const std::string& localName) {
		const uint64_t current = g_NetMatchService.GetLobbyMatchConfig().sessionId;
		if ((current && current != session) || snapshot.serviceState == "Idle") { names.clear(); seated.clear(); }
		if (current) session = current;
		for (const auto& member: snapshot.members) {
			std::string name = member.displayName;
			if ((member.isLocal || member.peerId == snapshot.localPeerId) && Placeholder(member.peerId, name) && !localName.empty()) name = localName;
			if (!Placeholder(member.peerId, name)) names[member.peerId] = name;
			if (!member.cpu && (member.connected || member.dropped || member.reclaiming || member.aiHeld)) seated.insert(member.peerId);
		}
	}

	/// Whether this session has seen the peer hold its seat.
	inline bool Seated(uint8_t peer) { return seated.contains(peer); }

	/// The departure every peer agrees on: the seat's own presence row, or the committed leave frame.
	inline bool Departed(uint8_t peer) {
		return g_NetMatchService.GetSeatPresence().StateOf(peer) == NetSeatPresenceState::Left ||
		    (Seated(peer) && ScenarioRunner::IsLockstepPeerGone(peer, ScenarioRunner::GetLockstepCompletedFrame()));
	}

	inline std::string Name(uint8_t peer, const std::string& fallback) {
		if (Placeholder(peer, fallback)) {
			if (const auto known = names.find(peer); known != names.end()) return known->second;
		}
		return fallback.empty() ? "Player " + std::to_string(peer) : fallback;
	}

	inline std::string Name(const NetLobbyMember& member) { return Name(member.peerId, member.displayName); }

	inline std::string State(uint8_t peer, bool aiHeld, bool dropped, bool reclaiming) {
		const uint64_t frame = ScenarioRunner::GetLockstepCompletedFrame();
		const bool left = Departed(peer);
		const bool ai = aiHeld || ScenarioRunner::IsLockstepSeatUnderAI(peer, frame) ||
		    (left && Seated(peer) && ScenarioRunner::IsLockstepPeerGone(peer, frame));
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
		// A seat that is gone has no live route to name.
		if (!member.connectedRoute.empty() && member.connected && !Departed(member.peerId)) row += " / via " + member.connectedRoute;
		return row;
	}
}
