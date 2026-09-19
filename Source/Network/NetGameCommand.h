#pragma once

#include <cstdint>
#include <map>
#include <array>
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
		SwitchControl = 8,
		AIEquip = 9,
		AIOrder = 10,
		Reseat = 11,
		SoundOp = 12,
		PlayerBindings = 13,
		AIScriptMessage = 14,
		AIGib = 15,
		PlaceBrain = 16, //!< 14 and 15 carry the AI intent commands.
		WorldTransition = 17,
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
		int32_t aiMode = -1; // -1 keeps the preset's default; >=0 sets Actor::AIMode after spawn.

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

	// Hand an actor's frame production to a teammate's peer: co-op players share a team, so which
	// machine drives each actor must cross the wire. A peer may only take control for itself.
	struct NetGameSwitchControl {
		int64_t actorUID = 0;
		int32_t team = 0;
		uint8_t newOwnerPeerId = 0;

		bool operator==(const NetGameSwitchControl&) const = default;
	};

	// An AI's equip call on an actor the issuing peer drives. The decision is per-machine (off-wire); the
	// call itself crosses the wire so every peer's sim, the owner's included, performs it at the committed tick.
	struct NetGameAIEquip {
		enum Op : uint8_t {
			Firearm = 0,
			DeviceInGroup = 1,
			LoadedFirearmInGroup = 2,
			NamedDevice = 3,
			Throwable = 4,
			DiggingTool = 5,
			Shield = 6,
			ShieldInBGArm = 7,
			UnequipFGArm = 8,
			UnequipBGArm = 9,
		};

		int64_t actorUID = 0;
		int32_t team = 0;
		uint8_t op = Firearm;
		bool depositToFront = false;
		std::string group;
		std::string excludeGroup;
		std::string moduleName;
		std::string presetName;

		bool operator==(const NetGameAIEquip&) const = default;
	};

	// A sound call an AI hook made on an actor the issuing peer drives. The decision is per-machine
	// (off-wire); the call crosses the wire so every peer's sim, the owner's included, performs it at
	// the committed tick, exactly like the equip calls.
	struct NetGameSoundOp {
		// Mirrors SoundContainer::PendingOp; MovableMan asserts the two stay in step.
		enum Op : uint8_t {
			Play = 0,
			Stop = 1,
			Restart = 2,
			FadeOut = 3,
			SelectSounds = 4,
			SetProperty = 5,
			AddSound = 6,
			RemoveSound = 7,
			AddSoundSet = 8,
			SetTopLevelSet = 9,
			SetCycleMode = 10,
			OpCount = 11
		};
		static constexpr uint8_t c_PropertyCount = 17;

		int64_t actorUID = 0;
		int32_t team = 0;
		uint64_t soundIdentity = 0;
		uint8_t op = 0;
		uint8_t property = 0;
		int32_t player = -1;
		int32_t value = 0;
		float x = 0.0F;
		float y = 0.0F;
		std::vector<uint16_t> soundSetPath;
		std::string payload; //!< A sound path or a SoundSet structure, for the calls that carry one.

		bool operator==(const NetGameSoundOp&) const = default;
	};

	// An order a player gives a unit of their team through the AI view modes: waypoints and squads. The
	// decision is the player's (off-wire); the writes land on every peer at the committed tick.
	struct NetGameAIOrder {
		enum Op : uint8_t {
			SceneWaypoint = 0,
			MOWaypoint = 1,
			ClearWaypoints = 2,
			FormSquad = 3,
			DisbandSquad = 4,
			PopWaypoint = 5,
			SetMOMoveTarget = 6,
			SetAlarmPoint = 7,
		};

		int64_t actorUID = 0;
		int32_t team = 0;
		uint8_t op = SceneWaypoint;
		float x = 0.0F;
		float y = 0.0F;
		int64_t targetUID = 0;
		int64_t writerUID = 0; //!< 0 = the target actor wrote its own queue.

		bool operator==(const NetGameAIOrder&) const = default;
	};

	// Hand a returning holder back the actors its seat controlled at the drop frame. System-authored: the
	// host issues it for a team it need not own, so every peer applies the identical ownership at one tick.
	struct NetGameReseat {
		int32_t team = 0;
		uint8_t newOwnerPeerId = 0;
		std::vector<int64_t> actorUIDs;

		bool operator==(const NetGameReseat&) const = default;
	};

	// A message an AI hook sent from inside its own pass. The decision is per-machine (off-wire), but the
	// receiving script runs on every peer, so the call crosses the wire and every peer delivers it at the
	// committed tick, exactly like the equip calls.
	struct NetGameAIScriptMessage {
		// What the message carries beside its name. A context no peer can name the same way (a table, a
		// function) is not one of these; that call stays on its producer and is reported at the boundary.
		enum Context : uint8_t {
			None = 0,
			Boolean = 1,
			Number = 2,
			Text = 3,
			Object = 4,
			ContextCount = 5
		};

		int64_t writerUID = 0; //!< The AI actor whose pass made the call; the authority for it.
		int64_t objectUID = 0; //!< The receiver.
		int32_t team = 0;
		uint8_t context = None;
		double number = 0.0;
		int64_t contextUID = 0;
		std::string message;
		std::string text;

		bool operator==(const NetGameAIScriptMessage&) const = default;
	};

	// A gib an AI hook asked for from inside its own pass. The gib spawns particles and takes the object out
	// of the world, which the producing boundary cannot undo, so the call crosses the wire and every peer,
	// the producer included, gibs at the committed tick.
	struct NetGameAIGib {
		int64_t writerUID = 0; //!< The AI actor whose pass made the call; the authority for it.
		int64_t objectUID = 0; //!< What to gib.
		int64_t ignoreUID = 0; //!< What the gibs may not hit; 0 is nothing.
		int32_t team = 0;
		float impulseX = 0.0F;
		float impulseY = 0.0F;

		bool operator==(const NetGameAIGib&) const = default;
	};

	/// A peer's local player reference, observed alongside its delayed input.
	struct NetPlayerBinding {
		bool active = false, human = false, hadBrain = false, brainEvacuated = false;
		int8_t team = -1;
		uint8_t viewState = 0;
		int64_t controlledUID = 0, brainUID = 0;
		float cameraX = 0, cameraY = 0;
		std::array<float, 8> viewTargets{};

		bool operator==(const NetPlayerBinding&) const = default;
	};

	// A seat's committed brain placement in the setup editor. Where to put the brain is the player's own
	// decision on their own machine (off-wire); the committed brain crosses so every peer installs the
	// identical resident from the named preset before the first seat starts the match.
	struct NetGamePlaceBrain {
		int32_t team = 0;
		int32_t player = -1; //!< The seat the brain belongs to; a team can hold several.
		float posX = 0.0F;
		float posY = 0.0F;
		std::string className;
		std::string preset;
		std::string module;

		bool operator==(const NetGamePlaceBrain&) const = default;
	};

	// Host-authored membership, spawn and binding for one announced tick.
	struct NetGameWorldTransition {
		enum Kind : uint8_t {
			Respawn = 0,  //!< Replace a team's lost resident; seats nobody.
			Activate = 1, //!< Seat a caught-up member at its announced tick.
			Release = 2,  //!< Free a cleanly left member's slot under its next generation.
			SeatRespawn = 3, //!< Give a seated member a new brain; the seat and its generation stay.
		};

		uint16_t schema = 1;           //!< World-plane schema; ordinary lockstep never carries this command.
		uint8_t kind = Respawn;
		uint8_t peerId = 0;            //!< The member this transition seats or frees; 0 on a respawn.
		uint32_t holderGeneration = 0; //!< The slot generation this member holds; a stale one is refused.
		uint64_t membershipRevision = 0;
		uint64_t activationFrame = 0;  //!< E: the first frame the member's input is required.
		int32_t team = 0;
		int32_t player = -1;           //!< The activity player slot to bind; -1 binds none.
		float posX = 0.0F;
		float posY = 0.0F;
		int32_t aiMode = -1;           //!< -1 keeps the preset's default.
		bool bindBrain = false;        //!< Make the spawned resident the member's brain.
		std::string className;         //!< Empty spawns nothing: a pure membership step.
		std::string preset;
		std::string module;

		bool operator==(const NetGameWorldTransition&) const = default;
	};

	/// Complete local slots; an empty slot clears the previous binding without changing the world.
	struct NetGamePlayerBindings {
		std::array<NetPlayerBinding, 4> players{};
		std::map<uint8_t, uint64_t> appliedCommands;
		bool operator==(const NetGamePlayerBindings&) const = default;
	};

	using NetGameCommandPayload = std::variant<NetGameSetTeamFunds, NetGameSpawnActor, NetGameDeliverCargo, NetGameScuttleCraft, NetGameInventoryOp, NetGamePauseMatch, NetGameSetActorAIMode, NetGameSwitchControl, NetGameAIEquip, NetGameAIOrder, NetGameReseat, NetGameSoundOp, NetGamePlayerBindings, NetGameAIScriptMessage, NetGameAIGib, NetGamePlaceBrain, NetGameWorldTransition>;

	struct NetGameCommand {
		uint8_t senderPeerId = 0;
		NetGameCommandPayload payload;
		uint64_t sequence = 0; //!< Sender-local identity retained when a pending command crosses a resync.

		bool operator==(const NetGameCommand&) const = default;
	};

	NetGameCommandType NetGameCommandTypeOf(const NetGameCommandPayload& payload);
	const char* NetGameCommandTypeName(NetGameCommandType type);
	int32_t NetGameCommandTeam(const NetGameCommandPayload& payload);

} // namespace RTE
