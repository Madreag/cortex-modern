#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace RTE {

	/// Bytes as megabytes with one decimal, the same on every machine whatever its locale.
	inline std::string NetMegabytesText(uint64_t bytes) {
		const uint64_t tenths = (bytes * 10 + 524288) / 1048576;
		return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
	}

	/// What a joiner reads while the world's image comes: how much of it, how fast and how long is left.
	inline std::string NetImageTransferLine(uint64_t received, uint64_t total, double bytesPerSecond) {
		std::string line = "Receiving the world: " + NetMegabytesText(received) + " of " + NetMegabytesText(total) + " MB";
		if (received >= total || !(bytesPerSecond >= 1.0)) return line;
		const uint64_t rate = static_cast<uint64_t>(bytesPerSecond);
		return line + " - " + NetMegabytesText(rate) + " MB/s - " + std::to_string((total - received + rate - 1) / rate) + " s left";
	}

	/// What a joiner waiting for a world's held slot to open reads while it knocks: the seconds its bounded wait has left.
	inline std::string NetSlotWaitLine(uint64_t leftMs) {
		// One lobby status row: the offer that led here already said why.
		return "Waiting for a slot to open - " + std::to_string((leftMs + 999) / 1000) + " s";
	}

	/// What a joiner reads while it replays the world up to the round: the frame it has reached of the one it needs, how fast it
	/// replays and how long is left. roundFramesPerSecond is how fast that target moves on, 0 when it is a fixed frame.
	inline std::string NetCatchUpLine(uint64_t applied, uint64_t target, double framesPerSecond, double roundFramesPerSecond) {
		std::string line = "Catching up with the world: frame " + std::to_string(applied) + " of " + std::to_string(target);
		const double closing = framesPerSecond - roundFramesPerSecond;
		if (applied >= target || !(framesPerSecond >= 1.0) || !(closing > 0.0)) return line;
		const uint64_t left = static_cast<uint64_t>(std::ceil(static_cast<double>(target - applied) / closing));
		return line + " - " + std::to_string(static_cast<uint64_t>(framesPerSecond)) + " frames/s - " + std::to_string(left) + " s left";
	}

	/// The host's line for a seat whose player is receiving the world's image or replaying it.
	inline std::string NetSeatJoinProgress(uint64_t receivedBytes, uint64_t totalBytes, bool catchingUp) {
		if (catchingUp) return "catching up";
		return "receiving the world " + NetMegabytesText(receivedBytes) + " of " + NetMegabytesText(totalBytes) + " MB";
	}

	/// The player's line when the host of the match it was joining is gone and the others play on under a new one.
	inline constexpr const char* c_NetMatchChangingHostLine = "The match is changing host - try again in a moment";

	/// The toast that announces a match repair: why it started and whose game was found out of step. The repair screen itself
	/// says that the match is resyncing, so this line never does. reason is the stop that asked for the repair.
	inline std::string NetRepairStartLine(const std::string& reason, const std::string& localName, const std::string& hostName, bool localIsHost) {
		const std::string started = "Match repair started";
		const std::string diverged = "sim state diverged at tick ";
		if (const size_t at = reason.find(diverged); at != std::string::npos) {
			const std::string rest = reason.substr(at + diverged.size());
			const std::string frame = rest.substr(0, rest.find_first_not_of("0123456789"));
			const size_t open = rest.find(" ("), close = rest.rfind(')');
			const std::string who = open != std::string::npos && close != std::string::npos && close > open + 2 ? rest.substr(open + 2, close - open - 2) : std::string();
			const std::string when = frame.empty() ? std::string() : " at frame " + frame;
			// The host's world is the one every peer reloads, so the host is never the one that fell out of step.
			if (!who.empty() && who == hostName) return started + (localIsHost ? ": a player fell out of step" : ": out of step with the host") + when;
			if (!who.empty() && who == localName) return started + ": your game fell out of step" + when;
			return started + ": " + (who.empty() ? std::string("a game") : who) + " fell out of step" + when;
		}
		const size_t requested = reason.find("ResyncRequested:");
		const std::string message = requested == std::string::npos ? std::string() : reason.substr(requested + 16);
		if (message.starts_with("host requested repair")) return started + (localIsHost ? " by you" : " by the host");
		if (message.starts_with("player rejoined")) return started + ": a player rejoined";
		if (message.starts_with("seat reclaimed")) return started + ": a held seat's player returned";
		if (message.starts_with("seat substituted")) return started + ": a substitute took a held seat";
		if (message.starts_with("successor snapshot")) return started + " after the host change";
		return started;
	}

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
		uint16_t inputDelayFrames = 0;
		uint32_t waits = 0;
		uint64_t longestWaitMs = 0;
		bool aiHeld = false;
		bool dropped = false;    //!< §11: the seat is held but its player's link is gone.
		bool reclaiming = false; //!< §11: that player is proving its ticket right now.
		bool joining = false;    //!< That player came in by admission and is still on its way into the round: joining, not rejoining.
		std::string connectedRoute;
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
		bool hostLost = false;
		bool migrating = false;
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
		bool leftMatch = false;     //!< This peer already sent the match its leave; the lobby that remains is not theirs.
		// A lobby that resumes a match from disk names the checkpoint it stands on, so every peer can
		// say whether it holds that very archive. Empty on an ordinary lobby.
		std::string resumeMatchId;
		uint32_t transferReceivedBytes = 0; //!< The world image this joiner is receiving: what has come so far.
		uint32_t transferTotalBytes = 0;    //!< Its whole size; 0 when no image is coming.
		std::string transferLine;           //!< The line the joiner reads while it comes; "" when none is.
		std::string waitLine;               //!< The line a joiner waiting for a world's held slot reads, with its countdown; "" when none is.
		bool joiningWorld = false;          //!< This peer comes into a running world by its image: nobody there readies up.
		uint64_t resumeTick = 0;
		std::string resumeDigest; //!< The checkpoint's world-structure digest, the identity a peer compares.
		bool resumeHeldLocally = false; //!< Whether this peer holds that checkpoint and will load its own copy.
		bool localReady = false;
		bool remoteReady = false;
		std::vector<NetLobbyMember> members;
	};

} // namespace RTE
