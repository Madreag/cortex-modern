#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace RTE {

	// An owner-issued, tick-stamped game command: the discrete sibling of the per-tick ControllerFrame. The
	// decision is off-wire; the committed action crosses from its owner and applies identically on both peers.
	enum class NetGameCommandType : uint16_t {
		SetTeamFunds = 1,
		SpawnActor = 2,
		DeliverCargo = 3,
		ScuttleCraft = 4,
		InventoryOp = 5,
		PauseMatch = 6,
		SetActorAIMode = 7,
	};

	// Set a team's funds to an exact value. Integer, trivially deterministic. Owner: the team owner.
	struct NetGameSetTeamFunds {
		int32_t team = 0;
		int32_t funds = 0;

		bool operator==(const NetGameSetTeamFunds&) const = default;
	};

	// Spawn an actor from a preset at an exact position for a team (deploy). Both peers clone it at the same
	// synced frame and sim state, so the new actor's unique id and physics match.
	struct NetGameSpawnActor {
		std::string className;
		std::string preset;
		std::string module;
		float posX = 0.0F;
		float posY = 0.0F;
		int32_t team = 0;

		bool operator==(const NetGameSpawnActor&) const = default;
	};

	// One manifest entry for a delivery: a preset to clone into the craft's hold before it ships.
	struct NetGameCargoItem {
		std::string className;
		std::string preset;
		std::string module;

		bool operator==(const NetGameCargoItem&) const = default;
	};

	// Deliver a craft loaded with a cargo manifest. The craft flies in under per-machine AI (off-wire); its
	// spawn, cargo, and physics are on-wire, applied identically on both peers. Owner: the team owner.
	struct NetGameDeliverCargo {
		std::string craftClassName;
		std::string craftPreset;
		std::string craftModule;
		float posX = 0.0F; // the landing zone when queuedPurchase
		float posY = 0.0F;
		int32_t team = 0;
		std::vector<NetGameCargoItem> cargo;
		// A committed buy-menu order: rides the single-player purchase core (arrival delay, passenger
		// nesting, funds deduction) instead of spawning the craft outright.
		bool queuedPurchase = false;
		float cost = 0.0F;
		bool returnCraft = true;
		int32_t passengerAIMode = 0;
		float waypointX = -1.0F;
		float waypointY = -1.0F;
		int64_t targetUID = 0;
		int8_t orderedByPlayer = -1; // display-only; honored only on the issuing peer
		float multiOrderYOffset = 0.0F;

		bool operator==(const NetGameDeliverCargo&) const = default;
	};

	// Scuttle (self-destruct) a craft the issuing peer's team controls. The gib runs in both peers' ungated
	// physics, so the trigger must cross the wire or only the owner gibs.
	struct NetGameScuttleCraft {
		int64_t actorUID = 0;
		int32_t team = 0;

		bool operator==(const NetGameScuttleCraft&) const = default;
	};

	// A player's inventory-menu action on an actor the issuing peer's team controls. The menu is per-machine
	// UI, so the committed mutation crosses the wire and applies on both peers at the same synced frame.
	struct NetGameInventoryOp {
		enum Op : uint8_t {
			SwapHands = 0,
			SwapEquipped = 1,
			Reorder = 2,
			Reload = 3,
			Drop = 4,
		};

		int64_t actorUID = 0;
		int32_t team = 0;
		uint8_t op = SwapHands;
		int16_t a = -1;
		int16_t b = -1;
		bool hasDropDirection = false;
		float dirX = 0.0F;
		float dirY = 0.0F;

		bool operator==(const NetGameInventoryOp&) const = default;
	};

	// Set an actor's AI mode from its owner. The AI decides per-machine (off-wire), but the mode is
	// sim state the craft death gates read, so the write must land on both peers.
	struct NetGameSetActorAIMode {
		int64_t actorUID = 0;
		int32_t team = 0;
		uint8_t aiMode = 0;

		bool operator==(const NetGameSetActorAIMode&) const = default;
	};

	// Pause or resume the match; both sims stop after the same synced frame and resume together
	// after a shared null-tick countdown. Owner: any peer, issued for its own team.
	struct NetGamePauseMatch {
		int32_t team = 0;
		bool pause = true;

		bool operator==(const NetGamePauseMatch&) const = default;
	};

	using NetGameCommandPayload = std::variant<NetGameSetTeamFunds, NetGameSpawnActor, NetGameDeliverCargo, NetGameScuttleCraft, NetGameInventoryOp, NetGamePauseMatch, NetGameSetActorAIMode>;

	struct NetGameCommand {
		uint8_t senderPeerId = 0;
		NetGameCommandPayload payload;

		bool operator==(const NetGameCommand&) const = default;
	};

	NetGameCommandType NetGameCommandTypeOf(const NetGameCommandPayload& payload);
	const char* NetGameCommandTypeName(NetGameCommandType type);
	int32_t NetGameCommandTeam(const NetGameCommandPayload& payload);

} // namespace RTE
