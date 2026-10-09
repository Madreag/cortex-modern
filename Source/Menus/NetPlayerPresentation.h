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

	/// The authoritative seat wins over cached connection flags, including this machine's own hold.
	inline bool OwnSeatHeld(const NetLobbySnapshot& snapshot, const std::optional<NetMatchService::SeatView>& view, bool released) {
		if (view) return view->seat.owner != 0 && (view->state == "Held" || view->state == "Reconnecting");
		if (released) return false;
		for (const auto& member: snapshot.members) {
			if (member.peerId == snapshot.localPeerId && !member.cpu && (member.aiHeld || member.reclaiming)) return true;
		}
		return false;
	}

	inline std::string PlayingSummary(bool ownHeld, size_t away, const std::string& awayName) {
		if (ownHeld) return "The AI is playing for you";
		return away == 0 ? "Everyone is playing" : away == 1 ? awayName + " is away" : std::to_string(away) + " players are away";
	}

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

	/// Whether the host opened the seat: its player no longer holds it.
	inline bool Opened(uint8_t peer) {
		const auto view = g_NetMatchService.GetSeatView(peer);
		return view && view->seat.owner == 0;
	}

	/// The departure every peer agrees on: the seat the host opened, or the committed leave frame.
	inline bool Departed(uint8_t peer) {
		return Opened(peer) || (Seated(peer) && ScenarioRunner::IsLockstepPeerGone(peer, ScenarioRunner::GetLockstepCompletedFrame()));
	}

	inline std::string Name(uint8_t peer, const std::string& fallback) {
		if (Placeholder(peer, fallback)) {
			if (const auto known = names.find(peer); known != names.end()) return known->second;
		}
		return fallback.empty() ? "Player " + std::to_string(peer) : fallback;
	}

	inline std::string Name(const NetLobbyMember& member) { return member.cpu ? (member.displayName.empty() ? "AI player" : member.displayName) : Name(member.peerId, member.displayName); }

	inline std::string State(uint8_t peer, bool aiHeld, bool dropped, bool reclaiming, bool joining = false) {
		const uint64_t frame = ScenarioRunner::GetLockstepCompletedFrame();
		// One read of the roster's seat answers every question below.
		const auto view = g_NetMatchService.GetSeatView(peer);
		const bool opened = view && view->seat.owner == 0;
		// The roster says who owns a place; the round's release reads only where the roster has no seat for the player, since a
		// place the host gave to a newcomer is released in the round until the newcomer is in.
		const bool released = view ? opened : ScenarioRunner::IsLockstepSeatReleased(peer);
		// A seat the AI plays for its player is held, however its player went; only the host's release makes it Left. The roster
		// keeps holding a place for its away player through the round and between rounds, where the round itself no longer reads it,
		// and for the player coming back or coming in until they are playing.
		const bool rosterHeld = view && !opened && (view->state == "Held" || view->state == "Reconnecting");
		const bool held = !released && (aiHeld || rosterHeld || ScenarioRunner::IsLockstepSeatUnderAI(peer, frame));
		const bool gone = Seated(peer) && ScenarioRunner::IsLockstepPeerGone(peer, frame);
		const bool left = !held && (opened || gone);
		const bool ai = held || (left && (ScenarioRunner::IsLockstepSeatUnderAI(peer, frame) || gone));
		if (left) return ai ? "Left - AI in control" : "Left";
		// Between rounds nobody plays the place, and it is kept for its player all the same.
		if (held && !reclaiming && g_NetMatchService.GetState() != NetMatchServiceState::Running) return "Held - the seat is kept";
		if (ai) return reclaiming ? (joining ? "Held - AI in control - joining" : "Held - AI in control - rejoining") : "Held - AI in control";
		if (reclaiming) return joining ? "Joining" : "Rejoining";
		if (dropped) return "Disconnected";
		return "Connected";
	}

	inline std::string State(const NetLobbyMember& member) {
		if (member.cpu) return "AI in control";
		return State(member.peerId, member.aiHeld, member.dropped, member.reclaiming, member.joining);
	}

	inline std::string Row(const NetLobbyMember& member) {
		// A seat nobody holds is open: no remembered name, and nothing reads it as connected.
		if (!member.connected && !member.cpu && !member.isLocal && !member.dropped && !member.reclaiming && !member.aiHeld && Placeholder(member.peerId, member.displayName)) return "Open seat";
		std::string row = Name(member) + "  /  Team " + std::to_string(member.team + 1) + "  /  " + State(member);
		// A seat that is gone has no live route to name.
		if (!member.cpu && !member.connectedRoute.empty() && member.connected && !Departed(member.peerId)) row += " / via " + member.connectedRoute;
		return row;
	}
}
