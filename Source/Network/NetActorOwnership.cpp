#include "NetActorOwnership.h"

#include "nlohmann/json.hpp"

namespace RTE {

	namespace {
		using json = nlohmann::json;

		uint8_t FindHumanPeerForTeam(const NetMatchConfig& config, uint8_t team) {
			for (const NetMatchPlayerSlot& player : config.players) {
				if (!player.cpu && player.team == team && player.peerId != 0 && player.peerId <= config.peerCount) {
					return player.peerId;
				}
			}
			return 0;
		}
	}

	uint8_t NetActorOwnership::ResolveOwnerPeer(const NetMatchConfig& config, const NetActorOwnershipQuery& query) {
		switch (config.ownershipPolicy) {
			case NetActorOwnershipPolicy::UniqueIdModPeerCount: {
				const uint8_t peerCount = config.peerCount > 0 ? config.peerCount : 1;
				const uint64_t absoluteId = query.actorUniqueID < 0 ? static_cast<uint64_t>(-(query.actorUniqueID + 1)) + 1ULL : static_cast<uint64_t>(query.actorUniqueID);
				return static_cast<uint8_t>(1 + (absoluteId % peerCount));
			}
			case NetActorOwnershipPolicy::TeamOwner: {
				if (const uint8_t teamPeer = FindHumanPeerForTeam(config, query.team)) {
					return teamPeer;
				}
				return config.hostPeerId;
			}
			case NetActorOwnershipPolicy::HostCpuRemoteHuman: {
				if (!query.cpuControlled) {
					if (const uint8_t teamPeer = FindHumanPeerForTeam(config, query.team)) {
						return teamPeer;
					}
				}
				return config.hostPeerId;
			}
		}
		return config.hostPeerId;
	}

	bool NetActorOwnership::IsLocalActor(const NetMatchConfig& config, uint8_t localPeerId, const NetActorOwnershipQuery& query) {
		return ResolveOwnerPeer(config, query) == localPeerId;
	}

	uint8_t NetActorOwnership::ResolveTeamCommandAuthority(const NetMatchConfig& config, uint8_t team) {
		if (config.players.empty()) {
			return 0;
		}
		if (const uint8_t teamPeer = FindHumanPeerForTeam(config, team)) {
			return teamPeer;
		}
		return config.hostPeerId;
	}

	NetActorOwnershipSummary NetActorOwnership::Summarize(const NetMatchConfig& config, const std::vector<NetActorOwnershipQuery>& actors) {
		NetActorOwnershipSummary summary;
		for (const NetActorOwnershipQuery& actor : actors) {
			const uint8_t owner = ResolveOwnerPeer(config, actor);
			if (owner == 0 || owner > config.peerCount) {
				++summary.unassignedActors;
			} else {
				++summary.actorsByPeer[owner];
			}
		}
		return summary;
	}

	std::string NetActorOwnership::BuildSummaryJson(const NetActorOwnershipSummary& summary) {
		json byPeer = json::object();
		for (const auto& [peer, count] : summary.actorsByPeer) {
			byPeer[std::to_string(peer)] = count;
		}
		json report = {
			{"actors_by_peer", std::move(byPeer)},
			{"unassigned_actors", summary.unassignedActors},
		};
		return report.dump();
	}

} // namespace RTE
