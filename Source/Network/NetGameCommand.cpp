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
		}
		return "Unknown";
	}

} // namespace RTE
