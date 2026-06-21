#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace RTE {

	// A discrete, owner-issued, tick-stamped game command: the sibling of the per-tick ControllerFrame. Under
	// Controller-sync the decision is off-wire (per-machine, like the AI); the committed action (funds, buy,
	// delivery, deploy, landing zone) crosses the wire from its owner and is applied identically on both peers
	// at the same frame, sorted by sender, so the on-wire result stays bit-identical.
	enum class NetGameCommandType : uint16_t {
		SetTeamFunds = 1,
		SpawnActor = 2,
	};

	// Set a team's funds to an exact value. Integer, trivially deterministic. Owner: the team owner.
	struct NetGameSetTeamFunds {
		int32_t team = 0;
		int32_t funds = 0;

		bool operator==(const NetGameSetTeamFunds&) const = default;
	};

	// Spawn an actor from a preset at an exact position for a team — the atom of deploy/buy/delivery. Owner: the
	// team owner. Both peers clone the same preset at the same synced frame and identical sim state, so the new
	// actor's unique id and physics match; its AI/controller stays off-wire.
	struct NetGameSpawnActor {
		std::string className;
		std::string preset;
		std::string module;
		float posX = 0.0F;
		float posY = 0.0F;
		int32_t team = 0;

		bool operator==(const NetGameSpawnActor&) const = default;
	};

	using NetGameCommandPayload = std::variant<NetGameSetTeamFunds, NetGameSpawnActor>;

	struct NetGameCommand {
		uint8_t senderPeerId = 0;
		NetGameCommandPayload payload;

		bool operator==(const NetGameCommand&) const = default;
	};

	NetGameCommandType NetGameCommandTypeOf(const NetGameCommandPayload& payload);
	const char* NetGameCommandTypeName(NetGameCommandType type);

} // namespace RTE
