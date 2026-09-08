#include "NetGameCommand.h"

#include <type_traits>

namespace RTE {

	NetGameCommandType NetGameCommandTypeOf(const NetGameCommandPayload& payload) {
		return std::visit([](const auto& specific) -> NetGameCommandType {
			using T = std::decay_t<decltype(specific)>;
			if constexpr (std::is_same_v<T, NetGameSetTeamFunds>) {
				return NetGameCommandType::SetTeamFunds;
			} else if constexpr (std::is_same_v<T, NetGameSpawnActor>) {
				return NetGameCommandType::SpawnActor;
			} else if constexpr (std::is_same_v<T, NetGameDeliverCargo>) {
				return NetGameCommandType::DeliverCargo;
			} else if constexpr (std::is_same_v<T, NetGameScuttleCraft>) {
				return NetGameCommandType::ScuttleCraft;
			} else if constexpr (std::is_same_v<T, NetGameInventoryOp>) {
				return NetGameCommandType::InventoryOp;
			} else if constexpr (std::is_same_v<T, NetGamePauseMatch>) {
				return NetGameCommandType::PauseMatch;
			} else if constexpr (std::is_same_v<T, NetGameSetActorAIMode>) {
				return NetGameCommandType::SetActorAIMode;
			} else if constexpr (std::is_same_v<T, NetGameSwitchControl>) {
				return NetGameCommandType::SwitchControl;
			} else if constexpr (std::is_same_v<T, NetGameAIEquip>) {
				return NetGameCommandType::AIEquip;
			} else if constexpr (std::is_same_v<T, NetGameAIOrder>) {
				return NetGameCommandType::AIOrder;
			} else if constexpr (std::is_same_v<T, NetGameReseat>) {
				return NetGameCommandType::Reseat;
			}
		}, payload);
	}

	int32_t NetGameCommandTeam(const NetGameCommandPayload& payload) {
		return std::visit([](const auto& specific) -> int32_t {
			return specific.team;
		}, payload);
	}

	const char* NetGameCommandTypeName(NetGameCommandType type) {
		switch (type) {
			case NetGameCommandType::SetTeamFunds:
				return "SetTeamFunds";
			case NetGameCommandType::SpawnActor:
				return "SpawnActor";
			case NetGameCommandType::DeliverCargo:
				return "DeliverCargo";
			case NetGameCommandType::ScuttleCraft:
				return "ScuttleCraft";
			case NetGameCommandType::InventoryOp:
				return "InventoryOp";
			case NetGameCommandType::PauseMatch:
				return "PauseMatch";
			case NetGameCommandType::SetActorAIMode:
				return "SetActorAIMode";
			case NetGameCommandType::SwitchControl:
				return "SwitchControl";
			case NetGameCommandType::AIEquip:
				return "AIEquip";
			case NetGameCommandType::AIOrder:
				return "AIOrder";
			case NetGameCommandType::Reseat:
				return "Reseat";
		}
		return "Unknown";
	}

} // namespace RTE
