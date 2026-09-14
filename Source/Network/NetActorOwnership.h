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

	/// An actor's owner as the world seeded it, with the team it was resolved at.
	struct NetSeededActorOwner {
		uint8_t ownerPeerId = 0;
		uint8_t team = 0;
	};

	class NetActorOwnership {
	public:
		static uint8_t ResolveOwnerPeer(const NetMatchConfig& config, const NetActorOwnershipQuery& query);

		/// Records the owner an actor was resolved to when it entered the world, and the team it was
		/// resolved at. The policy reads this first afterwards, so a control-mode change cannot move
		/// frame production under a live actor.
		/// @param actorUniqueID The actor's unique id.
		/// @param ownerPeerId The peer that owns it from now on.
		/// @param team The team the owner was resolved at.
		static void SeedOwner(int64_t actorUniqueID, uint8_t ownerPeerId, uint8_t team);
		static bool HasSeededOwner(int64_t actorUniqueID);
		/// @return Whether the actor's seeded owner was taken at this team.
		static bool HasSeededOwnerForTeam(int64_t actorUniqueID, uint8_t team);
		/// @return The seeded owner, or 0 if the actor has no entry.
		static uint8_t GetSeededOwner(int64_t actorUniqueID);
		/// @return The team the seeded owner was taken at, or 0 if the actor has no entry.
		static uint8_t GetSeededOwnerTeam(int64_t actorUniqueID);
		static const std::map<int64_t, NetSeededActorOwner>& GetSeededOwners();
		static void RestoreSeededOwners(std::map<int64_t, NetSeededActorOwner> owners);
		static void ClearSeededOwners();

		/// Resolves which peer is allowed to issue economy commands (funds, deploy, delivery) for a team:
		/// the human assigned to it, or the host for a CPU/unassigned team. 0 if the config defines no teams.
		/// @param config The synced match config.
		/// @param team The team the command targets.
		/// @return The authorized peer id, or 0 if team ownership is undefined.
		static uint8_t ResolveTeamCommandAuthority(const NetMatchConfig& config, uint8_t team);

		/// Whether a peer may issue team commands for the team: ANY of a shared team's human peers may
		/// (co-op teammates all buy for the team), or the host for a CPU/unassigned team.
		static bool IsTeamCommandAuthority(const NetMatchConfig& config, uint8_t team, uint8_t senderPeerId);
		static bool IsLocalActor(const NetMatchConfig& config, uint8_t localPeerId, const NetActorOwnershipQuery& query);
		static NetActorOwnershipSummary Summarize(const NetMatchConfig& config, const std::vector<NetActorOwnershipQuery>& actors);
		static std::string BuildSummaryJson(const NetActorOwnershipSummary& summary);
	};

} // namespace RTE
