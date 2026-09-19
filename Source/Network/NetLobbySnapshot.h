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
		std::string statusLine;  //!< §11's persistent line for the seat, derived on THIS peer; "" when the seat is fine.
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
		uint8_t hostPeerId = 1;
		int localTeam = -1;
		std::string serviceState;
		std::string statusText;
		std::string errorText;
		std::string lobbyPhase;
		std::string activityPreset;
		std::string activityModule; //!< The module the host's picker named, so a same-named preset cannot swap in.
		std::string sceneName;
		std::string sceneModule; //!< The module that defines the scene, when two modules share a name.
		std::string modeName;
		std::string modeLabel; //!< The friendly form of modeName, for labels that read a word, not a token.
		std::string inputDelayText; //!< The announced input delay, host-authored; "" before the lobby has one.
		std::string portMap;        //!< The host's router-mapping status line; "" when the toggle is off or not hosting.
		uint32_t portMapSerial = 0; //!< Bumped whenever portMap changes so the panel skips redundant rewrites.
		bool playedAMatch = false;  //!< A match has already run on this session, so this lobby is a rematch lobby.
		bool localReady = false;
		bool remoteReady = false;
		std::vector<NetLobbyMember> members;
	};

} // namespace RTE
