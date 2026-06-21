#pragma once

#include <cstdint>
#include <variant>

namespace RTE {

	// A discrete, owner-issued, tick-stamped game command: the sibling of the per-tick ControllerFrame. Under
	// Controller-sync the decision is off-wire (per-machine, like the AI); the committed action (funds, buy,
	// delivery, deploy, landing zone) crosses the wire from its owner and is applied identically on both peers
	// at the same frame, sorted by sender, so the on-wire result stays bit-identical.
	enum class NetGameCommandType : uint16_t {
		SetTeamFunds = 1,
	};

	// Set a team's funds to an exact value. Integer, trivially deterministic. Owner: the team owner.
	struct NetGameSetTeamFunds {
		int32_t team = 0;
		int32_t funds = 0;

		bool operator==(const NetGameSetTeamFunds&) const = default;
	};

	using NetGameCommandPayload = std::variant<NetGameSetTeamFunds>;

	struct NetGameCommand {
		uint8_t senderPeerId = 0;
		NetGameCommandPayload payload;

		bool operator==(const NetGameCommand&) const = default;
	};

	NetGameCommandType NetGameCommandTypeOf(const NetGameCommandPayload& payload);
	const char* NetGameCommandTypeName(NetGameCommandType type);

} // namespace RTE
