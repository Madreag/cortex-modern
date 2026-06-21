#pragma once

#include "NetMatchConfig.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace RTE {

	struct NetActorOwnershipQuery {
		int64_t actorUniqueID = 0;
		uint8_t team = 0;
		bool cpuControlled = false;
	};

	struct NetActorOwnershipSummary {
		std::map<uint8_t, uint32_t> actorsByPeer;
		uint32_t unassignedActors = 0;
	};

	class NetActorOwnership {
	public:
		static uint8_t ResolveOwnerPeer(const NetMatchConfig& config, const NetActorOwnershipQuery& query);
		static bool IsLocalActor(const NetMatchConfig& config, uint8_t localPeerId, const NetActorOwnershipQuery& query);
		static NetActorOwnershipSummary Summarize(const NetMatchConfig& config, const std::vector<NetActorOwnershipQuery>& actors);
		static std::string BuildSummaryJson(const NetActorOwnershipSummary& summary);
	};

} // namespace RTE
