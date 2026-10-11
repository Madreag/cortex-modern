#include "NetActorOwnership.h"
#include "CheckpointFrozenContainers.h"

#include "nlohmann/json.hpp"

namespace RTE {

	namespace {
		using json = nlohmann::json;

		std::map<int64_t, NetSeededActorOwner> s_SeededOwners; //!< Per-actor owners frozen at world entry, part of the world structure.
		struct SavedOwners : CheckpointNativeAllocated { CheckpointMap<int64_t, NetSeededActorOwner> values; bool ready = false; };
		std::unique_ptr<SavedOwners> s_SavedOwners;

		std::map<int64_t, NetSeededActorOwner>& SavedOwnerValues() {
			if (!s_SavedOwners) throw std::logic_error("the saved world has no actor owners");
			static const char kind = 0;
			const auto owners = CheckpointNativeStorage::ReadIndex(s_SavedOwners.get(), &kind, [] {
				const auto* source = CheckpointNativeStorage::Source(s_SavedOwners.get());
				if (!source->ready) throw std::logic_error("the saved actor owners were refused");
				auto result = std::make_shared<std::map<int64_t, NetSeededActorOwner>>();
				for (const auto& [actor, owner]: CheckpointValues(source->values)) result->emplace(actor, owner);
				return result;
			});
			return *const_cast<std::map<int64_t, NetSeededActorOwner>*>(static_cast<const std::map<int64_t, NetSeededActorOwner>*>(owners.get()));
		}

		void StoreSavedOwners() noexcept {
			if (!s_SavedOwners || !CheckpointNativeStorage::Enabled()) return;
			try {
				CheckpointFailure::Check(CheckpointFailure::Point::NativeObjects);
				CheckpointMap<int64_t, NetSeededActorOwner> values(s_SeededOwners.begin(), s_SeededOwners.end());
				s_SavedOwners->values.swap(values); s_SavedOwners->ready = true;
			} catch (const std::bad_alloc&) { s_SavedOwners->ready = false; }
		}

		uint8_t FindHumanPeerForTeam(const NetMatchConfig& config, uint8_t team) {
			for (const NetMatchPlayerSlot& player : config.players) {
				if (!player.cpu && player.team == team && player.peerId != 0 && player.peerId <= config.peerCount) {
					return player.peerId;
				}
			}
			return 0;
		}
	}

	void NetActorOwnership::SeedOwner(int64_t actorUniqueID, uint8_t ownerPeerId, uint8_t team) {
		if (CheckpointNativeStorage::Reading()) { SavedOwnerValues()[actorUniqueID] = {ownerPeerId, team}; return; }
		s_SeededOwners[actorUniqueID] = NetSeededActorOwner{ownerPeerId, team};
		if (s_SavedOwners && CheckpointNativeStorage::Enabled() && s_SavedOwners->ready) {
			try { s_SavedOwners->values[actorUniqueID] = {ownerPeerId, team}; }
			catch (const std::bad_alloc&) { s_SavedOwners->ready = false; }
		}
	}

	bool NetActorOwnership::HasSeededOwnerForTeam(int64_t actorUniqueID, uint8_t team) {
		const auto& owners = GetSeededOwners();
		const auto found = owners.find(actorUniqueID);
		return found != owners.end() && found->second.team == team;
	}

	uint8_t NetActorOwnership::GetSeededOwner(int64_t actorUniqueID) {
		const auto& owners = GetSeededOwners();
		const auto found = owners.find(actorUniqueID);
		return found != owners.end() ? found->second.ownerPeerId : uint8_t{0};
	}

	uint8_t NetActorOwnership::GetSeededOwnerTeam(int64_t actorUniqueID) {
		const auto& owners = GetSeededOwners();
		const auto found = owners.find(actorUniqueID);
		return found != owners.end() ? found->second.team : uint8_t{0};
	}

	const std::map<int64_t, NetSeededActorOwner>& NetActorOwnership::GetSeededOwners() {
		if (CheckpointNativeStorage::Reading()) return SavedOwnerValues();
		return s_SeededOwners;
	}

	void NetActorOwnership::RestoreSeededOwners(std::map<int64_t, NetSeededActorOwner> owners) {
		if (CheckpointNativeStorage::Reading()) { SavedOwnerValues() = std::move(owners); return; }
		s_SeededOwners = std::move(owners);
		StoreSavedOwners();
	}

	void NetActorOwnership::ClearSeededOwners() {
		if (CheckpointNativeStorage::Reading()) { SavedOwnerValues().clear(); return; }
		s_SeededOwners.clear();
		if (s_SavedOwners && CheckpointNativeStorage::Enabled()) { s_SavedOwners->values.clear(); s_SavedOwners->ready = true; }
	}

	void NetActorOwnership::PrepareCheckpointValues() {
		if (!CheckpointNativeStorage::Enabled()) return;
		if (!s_SavedOwners) s_SavedOwners = std::make_unique<SavedOwners>();
		StoreSavedOwners();
		if (!s_SavedOwners->ready) throw std::bad_alloc();
	}

	bool NetActorOwnership::CheckpointValuesReady() noexcept { return s_SavedOwners && s_SavedOwners->ready; }

	void NetActorOwnership::RetryCheckpointValues() noexcept {
		if (CheckpointNativeStorage::WorldEnabled() && !CheckpointNativeStorage::Reading() && !CheckpointValuesReady()) StoreSavedOwners();
	}

	std::string NetActorOwnership::CheckpointSelfTestMismatch() {
		CheckpointNativeStorage::AllocationScope allocations(true);
		struct Restore { std::map<int64_t, NetSeededActorOwner> values; ~Restore() { RestoreSeededOwners(std::move(values)); } } restore{s_SeededOwners};
		PrepareCheckpointValues(); ClearSeededOwners(); SeedOwner(71, 2, 3);
		const auto pages = CheckpointNativeStorage::Prepare();
		ClearSeededOwners(); SeedOwner(71, 1, 0); SeedOwner(73, 3, 1);
		{
			CheckpointNativeStorage::ReadScope read(pages);
			if (!HasSeededOwnerForTeam(71, 3) || GetSeededOwner(71) != 2 || GetSeededOwnerTeam(71) != 3 || GetSeededOwners().size() != 1)
				return "saved actor owners read a later live seed";
			SeedOwner(71, 3, 2); ClearSeededOwners(); RestoreSeededOwners({{75, {2, 1}}});
			if (GetSeededOwner(75) != 2 || GetSeededOwners().size() != 1) return "saved actor owner changes lost their private values";
		}
		if (GetSeededOwner(71) != 1 || GetSeededOwner(73) != 3 || GetSeededOwner(75) != 0) return "saved actor owners changed the running world";
		{
			CheckpointFailure::Scope refusal(CheckpointFailure::Point::NativeObjects);
			RestoreSeededOwners({{79, {3, 2}}});
			if (!refusal.Triggered() || CheckpointValuesReady() || GetSeededOwner(79) != 3) return "refused saved actor owners changed the live seed";
		}
		PrepareCheckpointValues();
		if (!CheckpointValuesReady()) return "saved actor owners could not retry a refused copy";
		const auto retry = CheckpointNativeStorage::Prepare();
		CheckpointNativeStorage::ReadScope read(retry);
		return GetSeededOwners().size() == 1 && GetSeededOwner(79) == 3 && GetSeededOwnerTeam(79) == 2 ? std::string() : "saved actor owner retry lost the latest seed";
	}

	uint8_t NetActorOwnership::ResolveOwnerPeer(const NetMatchConfig& config, const NetActorOwnershipQuery& query) {
		// The seed freezes the control-mode input at the team it was taken at; the actor's own team is
		// still an input, so a query at another team resolves from the policy again.
		const auto& owners = GetSeededOwners();
		if (const auto seeded = owners.find(query.actorUniqueID); seeded != owners.end() && seeded->second.team == query.team) {
			return seeded->second.ownerPeerId;
		}
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

	bool NetActorOwnership::IsTeamCommandAuthority(const NetMatchConfig& config, uint8_t team, uint8_t senderPeerId) {
		if (config.players.empty()) {
			return true;
		}
		bool teamHasHuman = false;
		for (const NetMatchPlayerSlot& player : config.players) {
			if (!player.cpu && player.team == team && player.peerId != 0 && player.peerId <= config.peerCount) {
				teamHasHuman = true;
				if (player.peerId == senderPeerId) {
					return true;
				}
			}
		}
		return !teamHasHuman && senderPeerId == config.hostPeerId;
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
