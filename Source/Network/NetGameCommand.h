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
		float posX = 0.0F;
		float posY = 0.0F;
		int32_t team = 0;
		std::vector<NetGameCargoItem> cargo;

		bool operator==(const NetGameDeliverCargo&) const = default;
	};

	using NetGameCommandPayload = std::variant<NetGameSetTeamFunds, NetGameSpawnActor, NetGameDeliverCargo>;

	struct NetGameCommand {
		uint8_t senderPeerId = 0;
		NetGameCommandPayload payload;

		bool operator==(const NetGameCommand&) const = default;
	};

	NetGameCommandType NetGameCommandTypeOf(const NetGameCommandPayload& payload);
	const char* NetGameCommandTypeName(NetGameCommandType type);

} // namespace RTE
