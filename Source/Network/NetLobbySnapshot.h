#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	/// A single lobby participant, as shown in the multiplayer lobby player list.
	struct NetLobbyMember {
		uint8_t peerId = 0;
		std::string displayName;
		uint8_t team = 0;
		bool cpu = false;
		bool isLocal = false;
		bool ready = false;
		bool connected = false;
		uint32_t pingMs = 0;
		bool dropped = false;    //!< §11: the seat is held but its player's link is gone.
		bool reclaiming = false; //!< §11: that player is proving its ticket right now.
	};

	/// A copyable snapshot of the multiplayer match/lobby state for the GUI thread to render. The service owns
	/// the top-level fields; the match runner publishes the lobby fields (roster, ready, ping) from its worker.
	struct NetLobbySnapshot {
		bool active = false;
		bool isHost = false;
		bool inLobby = false;
		bool running = false;
		bool failed = false;
		uint8_t localPeerId = 0;
		int localTeam = -1;
		std::string serviceState;
		std::string statusText;
		std::string errorText;
		std::string lobbyPhase;
		std::string activityPreset;
		std::string sceneName;
		std::string modeName;
		bool localReady = false;
		bool remoteReady = false;
		std::vector<NetLobbyMember> members;
	};

} // namespace RTE
