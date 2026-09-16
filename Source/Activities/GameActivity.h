#pragma once

#include <string>
#include <string_view>

/// Header file for the ActivityMan class.
/// @author Daniel Tabar
/// data@datarealms.com
/// http://www.datarealms.com
/// Inclusions of header files
#include "GUISound.h"
#include "RTETools.h"
#include "ActivityMan.h"
#include "Scene.h"
#include "Actor.h"

#include <array>
#include <memory>
#include <functional>
namespace RTE {

#define OBJARROWFRAMECOUNT 4
#define LZCURSORFRAMECOUNT 4

	class Actor;
	class ACraft;
	class PieMenu;
	class InventoryMenuGUI;
	class BuyMenuGUI;
	class SceneEditorGUI;
	class GUIBanner;
	class Loadout;

	/// Base class for all GameActivity:s, including game modes and editors.
	class GameActivity : public Activity {
		friend struct ContractAudit;


		friend struct ActivityLuaBindings;

		// Keeps track of everything about a delivery in transit after purchase has been made with the menu
		struct Delivery {
		friend struct ContractAudit;

			// OWNED by this until the delivery is made!
			ACraft* pCraft;
			// Which player ordered this delivery
			int orderedByPlayer;
			// Where to land
			Vector landingZone;
			// How much this delivery was offset upwards for multi-ordering, stored to help with delivery icons. If 0, this was presumably not a multi-order.
			float multiOrderYOffset;
			// How long left until entry, in ms
			long delay;
			// Times how long we've been in transit
			Timer timer;
		};

		/// Public member variable, method and friend function declarations
	public:
		void CaptureNetPlayerBindings(NetGamePlayerBindings& out) const override;
		bool CaptureNetLocalPlayerState(NetLocalPlayerState& out) const override;
		bool RestoreNetLocalPlayerState(const NetLocalPlayerState& state) override;
		bool ApplyNetPlayerBindings(const NetGamePlayerBindings& bindings) override;

		std::string SaveCheckpoint() const override;
		void VisitCheckpointOwnedObjects(const std::function<void(const Entity*)>& visit) const;
		/// Visits only the holdings every peer's activity owns identically, without the per-seat GUI ones.
		void VisitCheckpointSharedObjects(const std::function<void(const Entity*)>& visit) const;
		static bool RunDeliveryReferenceSelfTest();
		bool LoadCheckpoint(std::string_view text, bool validateOnly = false) override;
		bool ResolveCheckpointReferences() override;
		void ClearNonOwnedActorSlots() override;
		void RebindNonOwnedActorSlots() override;
		void ForgetDestroyedActor(const Actor* actor) override;

		/// Gives this player the first brain no seat has taken, the last step of brain placement.
		/// @param player The player to give a brain to.
		/// @return Whether the player has a brain now.
		bool PlaceUnassignedBrain(int player);

		/// Installs a seat's committed brain from the wire. Every peer builds the identical resident from
		/// the named preset at the named spot, so the brains that enter the sim match, and the seat counts
		/// as ready to start on every peer at the same frame.
		/// @return False, with nothing changed, when the placement names a seat, team, sender or preset this peer refuses.
		bool ApplyNetBrainPlacement(const NetGamePlaceBrain& placement, uint8_t senderPeerId);

		/// Commits one of this peer's own seats' brain placements to the wire. Where the brain goes is the
		/// player's own decision, read off their local editor; the command carries it to every peer.
		/// @return Whether the seat had a brain in a spot this machine accepts.
		bool SubmitLockstepBrainPlacement(int player);

		/// A brain spot every peer derives identically: the seat's landing zone at ground level. Used by a
		/// seat no peer drives and by the end-to-end arm that stands in for a player's placement.
		Vector DeterministicBrainSpot(int player) const;

		/// Commits a named brain at the seat's deterministic spot, for a seat no player is driving and for
		/// the end-to-end arm that stands in for a player's DONE.
		/// @return Whether the placement was committed.
		bool PlaceAndSubmitLockstepBrain(int player, const std::string& className, const std::string& preset, const std::string& module);

		/// Test-only seam: queues one scripted setup-editor gesture for a seat. "place_brain" holds the named
		/// brain over the ground at a fraction of the scene's width and presses; "done" presses DONE;
		/// "place_object" places a non-brain through the same editor; "actor_select" switches onto a craft
		/// passenger after the match starts. The gesture runs through the seat's own paths.
		/// @return Whether the gesture was queued.
		static bool QueueSetupEditorGesture(int player, const std::string& kind, float sceneXFraction, const std::string& className, const std::string& preset, const std::string& module);

		/// 0 = nothing queued for the seat, 1 = a gesture is still running, 2 = the seat could not carry it out.
		static int SetupEditorGestureStatus(int player);

		/// Whether this peer may write sim state for the seat (roster owner). True when lockstep is off.
		bool MayWriteLockstepSeat(int player) const;

		/// The in-game editor refused a write on this seat because the peer does not own it.
		static void NoteEditorWriteRefused(int player);
		static bool EditorWriteWasRefused(int player);

		/// Whether a seat has flagged itself ready to start.
		bool IsReadyToStart(int player) const { return player >= Players::PlayerOne && player < Players::MaxPlayerCount && m_ReadyToStart[player]; }

		/// Whether this peer has already committed the seat's placement; the commit is in flight until it returns.
		bool HasSubmittedLockstepPlacement(int player) const { return player >= Players::PlayerOne && player < Players::MaxPlayerCount && m_LockstepPlacementSubmitted[player]; }

		/// The seat's setup-editor mode, or -1 when the seat has no editor.
		int SetupEditorMode(int player) const;

		/// Presentation only: who the synchronized setup editor is still waiting on, and how many seats have
		/// placed. Read by the match status strip; never read by the simulation.
		/// @return Whether the match is holding in the synchronized setup editor.
		bool DescribeLockstepPlacementWait(std::string& names, int& placed, int& total) const;

		/// Test-only seam: puts a placement command on the wire exactly as issued, so a command every peer
		/// has to refuse - an unknown preset, a seat this peer does not hold - can be exercised end to end.
		/// @return Whether the command was enqueued.
		static bool EnqueueRawBrainPlacement(int player, int team, float posX, float posY, const std::string& className, const std::string& preset, const std::string& module);

		void ClearCheckpointActorIDs() override;
		bool PrepareCheckpointUI() override;
		SerializableOverrideMethods;
		ClassInfoGetters;

		enum ObjectiveArrowDir {
			ARROWDOWN = 0,
			ARROWLEFT,
			ARROWRIGHT,
			ARROWUP
		};

		enum BannerColor {
			RED = 0,
			YELLOW
		};

		/// Constructor method used to instantiate a GameActivity object in system
		/// memory. Create() should be called before using the object.
		GameActivity();

		/// Destructor method used to clean up a GameActivity object before deletion
		/// from system memory.
		~GameActivity() override;

		/// Makes the GameActivity object ready for use.
		/// @return An error return value signaling sucess or any particular failure.
		/// Anything below 0 is an error signal.
		int Create() override;

		/// Creates a GameActivity to be identical to another, by deep copy.
		/// @param reference A reference to the GameActivity to deep copy.
		/// @return An error return value signaling sucess or any particular failure.
		/// Anything below 0 is an error signal.
		int Create(const GameActivity& reference);

		/// Resets the entire GameActivity, including its inherited members, to their
		/// default settings or values.
		void Reset() override {
			Clear();
			Activity::Reset();
		}

		/// Destroys and resets (through Clear()) the GameActivity object.
		/// @param notInherited Whether to only destroy the members defined in this derived class, or (default: false)
		/// to destroy all inherited members also.
		void Destroy(bool notInherited = false) override;

		/// Gets the current CPU-assisted team, if any (NoTeam) - LEGACY function
		/// @return The current setting. NoTeam is no team is assisted.
		int GetCPUTeam() const { return m_CPUTeam; }

		/// Sets the current CPU-assisted team, if any (NoTeam) - LEGACY function
		/// @param team The new setting. NoTeam is no team is assisted. (default: Activity::NoTeam)
		void SetCPUTeam(int team = Activity::NoTeam);
		void ConfigureLockstepCPUTeams(const std::array<bool, Teams::MaxTeamCount>& cpuTeams);

		/// Sets the observation sceneman scroll targets, for when the game is
		/// over or a player is in observation mode
		/// @param newTarget The new absolute position to observe.
		/// @param player Which player to set it for. (default: 0)
		void SetObservationTarget(const Vector& newTarget, int player = 0) {
			if (LocalInputOfPlayer(player) != Players::NoPlayer)
				m_ObservationTarget[player] = newTarget;
		}

		/// The observation scroll target for a seat.
		const Vector& GetObservationTarget(int player = 0) const {
			return (player >= Players::PlayerOne && player < Players::MaxPlayerCount) ? m_ObservationTarget[player] : m_ObservationTarget[Players::PlayerOne];
		}

		/// Sets the player death sceneman scroll targets, for when a player-
		/// controlled actor dies and the view should go to his last position
		/// @param newTarget The new absolute position to set as death view.
		/// @param player Which player to set it for. (default: 0)
		void SetDeathViewTarget(const Vector& newTarget, int player = 0) {
			if (LocalInputOfPlayer(player) != Players::NoPlayer)
				m_DeathViewTarget[player] = newTarget;
		}

		/// Sets the he last selected landing zone.
		/// @param newZone The new absolute position to set as the last selected landing zone.
		/// @param player Which player to set it for. (default: 0)
		void SetLandingZone(const Vector& newZone, int player = 0) {
			if (LocalInputOfPlayer(player) != Players::NoPlayer)
				m_LandingZone[player] = newZone;
		}

		/// Gets the he last selected landing zone.
		/// @param player Which player to get it for. (default: 0)
		/// @return The new absolute position to set as the last selected landing zone.
		Vector GetLandingZone(int player = 0) {
			if (LocalInputOfPlayer(player) != Players::NoPlayer)
				return m_LandingZone[player];
			else
				return Vector();
		}

		/// Sets the actor selection cursor position.
		/// @param newPos The new absolute position to put the cursor at.
		/// @param player Which player to set it for. (default: 0)
		void SetActorSelectCursor(const Vector& newPos, int player = 0) {
			if (LocalInputOfPlayer(player) != Players::NoPlayer)
				m_ActorCursor[player] = newPos;
		}

		/// Gets the an in-game GUI Object for a specific player.
		/// @param which Which player to get the GUI for. (default: 0)
		/// @return A pointer to a BuyMenuGUI. Ownership is NOT transferred!
		BuyMenuGUI* GetBuyGUI(unsigned int which = 0) const;

		/// Checks if the in-game GUI Object is visible for a specific player.
		/// @param which Which player to check the GUI for. -1 will check all players.
		/// @return Whether or not the BuyMenuGUI is visible for input player(s).
		bool IsBuyGUIVisible(int which = 0) const;

		/// Gets the an in-game editor GUI Object for a specific player.
		/// @param which Which player to get the GUI for. (default: 0)
		/// @return A pointer to a SceneEditorGUI. Ownership is NOT transferred!
		SceneEditorGUI* GetEditorGUI(unsigned int which = 0) const;
		static bool RunNetLocalUIRestoreSelfTest();
		static bool RunNetInventoryRelaunchProbe(std::string_view phase);

		/// Locks a player controlled actor to a specific controller mode.
		/// Locking the actor will disable player input, including switching actors.
		/// @param player Which player to lock the actor for.
		/// @param lock Whether to lock or unlock the actor. (Default: true)
		/// @param lockToMode Which controller mode to lock the actor to. (Default: `CIM_AI`)
		/// @return Whether the (un)lock was performed.
		bool LockControlledActor(Players player, bool lock = true, Controller::InputMode lockToMode = Controller::InputMode::CIM_AI);

		/// Forces the this to focus player control to a specific Actor for a
		/// specific team. OWNERSHIP IS NOT TRANSFERRED!
		/// @param pActor Which Actor to switch focus to. The team of this Actor will be set
		/// once it is passed in. Ownership IS NOT TRANSFERRED! The Actor should
		/// be added to MovableMan already.
		/// @return Whether the focus switch was successful or not.
		bool SwitchToActor(Actor* pActor, int player = 0, int team = 0) override;

		/// Forces the this to focus player control to the next Actor of a
		/// specific team, other than the current one focused on.
		/// @param player Which team to switch to next actor on.
		/// @param team An actor pointer to skip in the sequence.
		void SwitchToNextActor(int player, int team, Actor* pSkip = 0) override;

		/// Forces this to focus player control to the previous Actor of a
		/// specific team, other than the current one focused on.
		/// @param player Which team to switch to next actor on.
		/// @param team An actor pointer to skip in the sequence.
		void SwitchToPrevActor(int player, int team, Actor* pSkip = 0) override;

		/// Sets which team is the winner, when the game is over.
		/// @param winnerTeam The team number of the winning team. 0 is team #1. Negative number
		/// means the game isn't over yet.
		void SetWinnerTeam(int winnerTeam) { m_WinnerTeam = winnerTeam; }

		/// Indicates which team is the winner, when the game is over.
		/// @return The team number of the winning team. 0 is team #1. Negative number
		/// means the game isn't over yet.
		int GetWinnerTeam() const { return m_WinnerTeam; }

		/// Gets access to the huge banner of any player that can display
		/// messages which can not be missed or ignored.
		/// @param whichColor Which color banner to get - see the GameActivity::BannerColor enum. (default: YELLOW)
		/// @param player Which player's banner to get. (default: Players::PlayerOne)
		/// @return A pointer to the GUIBanner object that we can
		GUIBanner* GetBanner(int whichColor = YELLOW, int player = Players::PlayerOne) const;

		/// Sets the Area within which a team can land things.
		/// @param team The number of the team we're setting for.
		/// @param newArea The Area we're setting to limit their landings within.
		void SetLZArea(int team, const Scene::Area& newArea) {
			if (team < Teams::TeamOne || team >= Teams::MaxTeamCount) return;
			m_LandingZoneArea[team].Reset();
			m_LandingZoneArea[team].Create(newArea);
		}

		/// Gets the Area within which a team can land things. OWNERSHIP IS NOT TRANSFERRED!
		/// @param team The number of the team we're setting for.
		/// @return The Area we're using to limit their landings within. OWNERSHIP IS NOT TRANSFERRED!
		const Scene::Area& GetLZArea(int team) const {
			return team >= Teams::TeamOne && team < Teams::MaxTeamCount ? m_LandingZoneArea[team] : s_NoLandingZone;
		}

		/// Sets the width of the landing zone box that follows around a player's
		/// brain.
		/// @param player The number of the in-game player we're setting for.
		/// @param width The width of the box, in pixels. 0 means disabled.
		void SetBrainLZWidth(int player, int width) { if (LocalInputOfPlayer(player) != Players::NoPlayer) m_BrainLZWidth[player] = width; }

		/// Gets the width of the landing zone box that follows around a player's
		/// brain.
		/// @param player The number of the player we're getting for.
		/// @return The width in pixels of the landing zone.
		int GetBrainLZWidth(int player) const { return LocalInputOfPlayer(player) != Players::NoPlayer ? m_BrainLZWidth[player] : 0; }

		/// Created an objective point for one of the teams to show until cleared.
		/// @param description The team number of the team to give objective. 0 is team #1.
		/// @param objPos The very short description of what the objective is (three short words max)
		/// @param whichTeam The absolute scene coordiante position of the objective. (default: Teams::TeamOne)
		/// @param arrowDir The desired direction of the arrow when the point is on screen. (default: ARROWDOWN)
		void AddObjectivePoint(const std::string& description, Vector objPos, int whichTeam = Teams::TeamOne, ObjectiveArrowDir arrowDir = ARROWDOWN);

		/// Sorts all objective points according to their positions on the Y axis.
		void YSortObjectivePoints();

		/// Clears all objective points previously added, for both teams.
		void ClearObjectivePoints() { m_Objectives.clear(); }

		/// Adds somehting to the purchase list that will override what is set
		/// in the buy guy next time CreateDelivery is called.
		/// @param pPurchase The SceneObject preset to add to the override purchase list. OWNERSHIP IS NOT TRANSFERRED!
		/// @param player Which player's list to add an override purchase item to.
		/// @return The new total value of what's in the override purchase list.
		int AddOverridePurchase(const SceneObject* pPurchase, int player);

		/// First clears and then adds all the stuff in a Loadout to the override
		/// purchase list.
		/// @param pLoadout The Loadout preset to set the override purchase list to reflect. OWNERSHIP IS NOT TRANSFERRED!
		/// @param player The player we're talking about.
		/// @return The new total value of what's in the override purchase list.
		int SetOverridePurchaseList(const Loadout* pLoadout, int player);

		/// First clears and then adds all the stuff in a Loadout to the override
		/// purchase list.
		/// @param loadoutName The name of the Loadout preset to set the override purchase list to
		/// represent.
		/// @return The new total value of what's in the override purchase list.
		int SetOverridePurchaseList(const std::string& loadoutName, int player);

		/// Clears all items from a specific player's override purchase list.
		/// @param m_PurchaseOverride[player].clear( Which player's override purchase list to clear.
		void ClearOverridePurchase(int player) { if (LocalInputOfPlayer(player) != Players::NoPlayer) m_PurchaseOverride[player].clear(); }

		/// Takes the current order out of a player's buy GUI, creates a Delivery
		/// based off it, and stuffs it into that player's delivery queue.
		/// @param player Which player to create the delivery for. Cargo AI mode and waypoint.
		/// @return Success or not.
		bool CreateDelivery(int player, int mode, Vector& waypoint) { return CreateDelivery(player, mode, waypoint, NULL); };

		/// Takes the current order out of a player's buy GUI, creates a Delivery
		/// based off it, and stuffs it into that player's delivery queue.
		/// @param player Which player to create the delivery for. Cargo AI mode and TargetMO.
		/// @return Success or not.
		bool CreateDelivery(int player, int mode, Actor* pTargetMO) {
			Vector point(-1, -1);
			return CreateDelivery(player, mode, point, pTargetMO);
		};

		/// Takes the current order out of a player's buy GUI, creates a Delivery
		/// based off it, and stuffs it into that player's delivery queue.
		/// @param player Which player to create the delivery for and Cargo AI mode.
		/// @return Success or not.
		bool CreateDelivery(int player, int mode) {
			Vector point(-1, -1);
			return CreateDelivery(player, mode, point, NULL);
		};

		/// Takes the current order out of a player's buy GUI, creates a Delivery
		/// based off it, and stuffs it into that player's delivery queue.
		/// @param player Which player to create the delivery for.
		/// @return Success or not.
		bool CreateDelivery(int player) {
			Vector point(-1, -1);
			return CreateDelivery(player, Actor::AIMODE_SENTRY, point, NULL);
		};

		/// One committed purchase: everything QueuePurchaseDelivery needs to build and queue the delivery.
		struct PurchaseOrder {
		friend struct ContractAudit;

			std::list<const SceneObject*> purchases; //!< Item presets to clone into the craft; not owned.
			int team = Teams::NoTeam;
			int passengerAIMode = Actor::AIMODE_SENTRY;
			Vector waypoint = Vector(-1, -1);
			Actor* pTargetMO = nullptr;
			float totalCost = 0.0F;
			int orderedByPlayer = Players::NoPlayer;
			bool aiReturnCraft = true;
			Vector landingZone;
			float multiOrderYOffset = 0.0F;
		};

		/// Nests a purchase order into the delivery craft, queues the craft for arrival over the landing
		/// zone, and deducts the cost from the team's funds. Takes craft ownership on success.
		/// @return Whether the delivery was queued.
		bool QueuePurchaseDelivery(ACraft* pDeliveryCraft, const PurchaseOrder& order);

		/// Shows how many deliveries this team has pending.
		/// @param m_Deliveries[team].size( Which team to check the delivery count for.
		/// @return The number of deliveries this team has coming.
		int GetDeliveryCount(int team) { return team >= Teams::TeamOne && team < Teams::MaxTeamCount ? m_Deliveries[team].size() : 0; }

		/// Precalculates the player-to-screen index map, counts the number of
		/// active players etc.
		void SetupPlayers() override;

		/// Officially starts the game accroding to parameters previously set.
		/// @return An error return value signaling sucess or any particular failure.
		/// Anything below 0 is an error signal.
		int Start() override;

		/// Pauses and unpauses the game.
		/// @param pause Whether to pause the game or not. (default: true)
		void SetPaused(bool pause = true) override;

		/// Forces the current game's end.
		void End() override;

		/// This is a special update step for when any player is still editing the
		/// scene.
		void UpdateEditing();




		virtual void Update();

		/// Updates the render/realtime state of this Activity. Supposed to be done every frame before drawing.
		virtual void RenderUpdate();

		/// Draws the currently active GUI of a screen to a BITMAP of choice.
		/// @param pTargetBitmap A pointer to a screen-sized BITMAP to draw on.
		/// @param targetPos The absolute position of the target bitmap's upper left corner in the scene. (default: Vector())
		/// @param which Which screen's GUI to draw onto the bitmap. (default: 0)
		void DrawGUI(BITMAP* pTargetBitmap, const Vector& targetPos = Vector(), int which = 0) override;

		/// Draws this ActivityMan's current graphical representation to a
		/// BITMAP of choice. This includes all game-related graphics.
		/// @param pTargetBitmap A pointer to a BITMAP to draw on. OWNERSHIP IS NOT TRANSFERRED!
		/// @param targetPos The absolute position of the target bitmap's upper left corner in the scene. (default: Vector())
		void Draw(BITMAP* pTargetBitmap, const Vector& targetPos = Vector()) override;

		/// Returns the name of the tech module selected for this team during scenario setup
		/// @param team Team to return tech module for
		/// @return Tech module name, for example Dummy.rte, or empty string if there is no team
		std::string GetTeamTech(int team) const { return (team >= Teams::TeamOne && team < Teams::MaxTeamCount) ? m_TeamTech[team] : ""; }

		/// Sets tech module name for specified team. Module must set must be loaded.
		/// @param team Team to set module, module name, for example Dummy.rte
		void SetTeamTech(int team, const std::string& tech);

		/// Indicates whether a specific team is assigned a CPU player in the current game.
		/// @param team Which team index to check.
		/// @return Whether the team is assigned a CPU player in the current activity.
		bool TeamIsCPU(int team) const { return (team >= Teams::TeamOne && team < Teams::MaxTeamCount) ? m_TeamIsCPU[team] : false; }

		/// Returns active CPU team count.
		/// @return Returns active CPU team count.
		int GetActiveCPUTeamCount() const;

		/// Returns active human team count.
		/// @return Returns active human team count.
		int GetActiveHumanTeamCount() const;

		/// Changes how much starting gold was selected in scenario setup dialog. 20000 - infinite amount.
		/// @param amount Starting gold amount
		void SetStartingGold(int amount) { m_StartingGold = amount; }

		/// Returns how much starting gold was selected in scenario setup dialog. 20000 - infinite amount.
		/// @return How much starting gold must be given to human players.
		int GetStartingGold() { return m_StartingGold; }

		/// Changes whether fog of war must be enabled for this activity or not.
		/// Never hides or reveals anything, just changes internal flag.
		/// @param enable New fog of war state. true = enabled.
		/// Return value:	None.
		void SetFogOfWarEnabled(bool enable) { m_FogOfWarEnabled = enable; }

		/// Returns whether fog of war must be enabled for this activity or not.
		/// Call it to determine whether you should call MakeAllUnseen or not at the start of activity.
		/// @return Whether Fog of war flag was checked during scenario setup dialog.
		bool GetFogOfWarEnabled() { return m_FogOfWarEnabled; }

		/// Tells whether player activity requires a cleat path to orbit to place brain
		/// Return value:	Whether we need a clear path to orbit to place brains.
		bool GetRequireClearPathToOrbit() const { return m_RequireClearPathToOrbit; }

		/// Tells whether player activity requires a cleat path to orbit to place brain
		/// @param newvalue Whether we need a clear path to orbit to place brains.
		/// Return value:	None.
		void SetRequireClearPathToOrbit(bool newvalue) { m_RequireClearPathToOrbit = newvalue; }

		int GetDefaultFogOfWar() const { return m_DefaultFogOfWar; }

		int GetDefaultRequireClearPathToOrbit() const { return m_DefaultRequireClearPathToOrbit; }

		int GetDefaultDeployUnits() const { return m_DefaultDeployUnits; }

		int GetDefaultGoldCakeDifficulty() const { return m_DefaultGoldCakeDifficulty; }

		int GetDefaultGoldEasyDifficulty() const { return m_DefaultGoldEasyDifficulty; }

		int GetDefaultGoldMediumDifficulty() const { return m_DefaultGoldMediumDifficulty; }

		int GetDefaultGoldHardDifficulty() const { return m_DefaultGoldHardDifficulty; }

		int GetDefaultGoldNutsDifficulty() const { return m_DefaultGoldNutsDifficulty; }

		/// Gets the default gold for max difficulty.
		/// @return The default gold for max difficulty.
		int GetDefaultGoldMaxDifficulty() const { return m_DefaultGoldMaxDifficulty; }

		bool GetFogOfWarSwitchEnabled() const { return m_FogOfWarSwitchEnabled; }

		bool GetDeployUnitsSwitchEnabled() const { return m_DeployUnitsSwitchEnabled; }

		bool GetGoldSwitchEnabled() const { return m_GoldSwitchEnabled; }

		bool GetRequireClearPathToOrbitSwitchEnabled() const { return m_RequireClearPathToOrbitSwitchEnabled; }

		bool GetTeamTechSwitchEnabled(int team) const { return m_TeamTechSwitchEnabled[team]; }

		/// Returns CrabToHumanSpawnRatio for specified module
		/// @return Crab-To-Human spawn ratio value set for specified module, 0.25 is default.
		float GetCrabToHumanSpawnRatio(int moduleid);

		/// Returns current delivery delay
		/// @return Returns current delivery delay
		long GetDeliveryDelay() const { return m_DeliveryDelay; }

		/// Sets delivery delay
		/// @param newDeliveryDelay New delivery delay value in ms
		void SetDeliveryDelay(long newDeliveryDelay) { m_DeliveryDelay = newDeliveryDelay > 1 ? newDeliveryDelay : 1; }

		/// Returns whether buy menu is enabled in this activity.
		/// @param True if buy menu enabled false otherwise
		bool GetBuyMenuEnabled() const { return m_BuyMenuEnabled; }

		/// Sets whether buy menu is enabled in this activity
		/// @param newValue True to enable buy menu, false otherwise
		void SetBuyMenuEnabled(bool newValue) { m_BuyMenuEnabled = newValue; }

		/// Returns network player name
		/// @param player Player
		/// @return Network player name
		const std::string& GetNetworkPlayerName(int player);

		/// Sets network player name
		/// @param player Player number, player name
		void SetNetworkPlayerName(int player, std::string name);

		/// Protected member variable and method declarations
	protected:
		/// Runs a brainless human's spectator view: actor cycling, following and the followed-unit line.
		/// Presentation only - it writes this peer's view, never simulation state.
		/// @param player Which player's screen to update.
		/// @param lookedAround Whether the player moved the observation cursor this frame.
		void UpdateSpectatorView(int player, bool lookedAround);

		/// Takes the current order out of a player's buy GUI, creates a Delivery
		/// based off it, and stuffs it into that player's delivery queue.
		/// @param player Which player to create the delivery for. Cargo AI mode waypoint or TargetMO.
		/// @return Success or not.
		bool CreateDelivery(int player, int mode, Vector& waypoint, Actor* pTargetMO);

		/// A struct to keep all data about a mission objective.
		struct ObjectivePoint {
			std::string SaveCheckpoint() const;
			bool LoadCheckpoint(std::string_view text, bool validateOnly = false);
		friend struct ContractAudit;

			ObjectivePoint() {
				m_Description.clear();
				m_ScenePos.Reset();
				m_Team = Teams::NoTeam;
				m_ArrowDir = ARROWDOWN;
			}

			ObjectivePoint(std::string desc, const Vector& pos, int team = -1, ObjectiveArrowDir arrowDir = ARROWDOWN) :
				m_Description(std::move(desc)),
				m_ScenePos(pos),
				m_Team((Teams)team),
				m_ArrowDir(arrowDir)
			{}

			/// Simply draws this' arrow relative to a point on a bitmap.
			/// @param pTargetBitmap A pointer to the BITMAP to draw on.
			/// @param pArrowBitmap The arrow bitmap to draw, assuming it points downward.
			/// @param arrowPoint The absolute position on the bitmap to draw the point of the arrow at.
			/// @param arrowDir Which orientation to draw the arrow in, relative to the point. (default: ARROWDOWN)
			void Draw(BITMAP* pTargetBitmap, BITMAP* pArrowBitmap, const Vector& arrowPoint, ObjectiveArrowDir arrowDir = ARROWDOWN);

			// The description of this objective point
			std::string m_Description;
			// Absolute position in the scene where this is pointed
			Vector m_ScenePos;
			// The team this objective is relevant to
			Teams m_Team;
			// The positioning of the arrow that points at this objective
			ObjectiveArrowDir m_ArrowDir;
		};

		// Comparison functor for sorting objective points by their y pos using STL's sort
		struct ObjPointYPosComparison {
			bool operator()(ObjectivePoint& rhs, ObjectivePoint& lhs) { return rhs.m_ScenePos.m_Y < lhs.m_ScenePos.m_Y; }
		};

		/// Gets the next other team number from the one passed in, if any. If there
		/// are more than two teams in this game, then the next one in the series
		/// will be returned here.
		/// @param team The team not to get.
		/// @return The other team's number.
		int OtherTeam(int team);

		/// Indicates whether there is less than two teams left in this game with
		/// a brain in its ranks.
		/// @return Whether less than two teams have brains in them left.
		bool OneOrNoneTeamsLeft();

		/// Indicates which single team is left, if any.
		/// @return Which team stands alone with any brains in its ranks, if any. NoTeam
		/// is returned if there's either more than one team, OR there are no
		/// teams at all left with brains in em.
		int WhichTeamLeft();

		/// Indicates whether there are NO teams left with any brains at all!
		/// @return Whether any team has a brain in it at all.
		bool NoTeamLeft();

		/// Goes through all Actor:s currently in the MovableMan and sets each
		/// one not controlled by a player to be AI controlled and AIMode setting
		/// based on team and CPU team.
		virtual void InitAIs();

		/// Goes through all Actor:s currently in the MovableMan and disables or
		/// enables each one with a Controller set to AI input.
		/// @param disable Whether to disable or enable them; (default: true)
		/// @param whichTeam Which team to do this to. If all, then pass Teams::NoTeam (default: Teams::NoTeam)
		void DisableAIs(bool disable = true, int whichTeam = Teams::NoTeam);

		// Member variables
		static Entity::ClassInfo m_sClass;

		// Which team is CPU-managed, if any (-1) - LEGACY, now controlled by Activity::m_IsHuman
		int m_CPUTeam;
		// Team is active or not this game
		bool m_TeamIsCPU[Teams::MaxTeamCount];

		// The observation sceneman scroll targets, for when the game is over or a player is in observation mode
		Vector m_ObservationTarget[Players::MaxPlayerCount];
		bool m_ObserveFreezeHeld[Players::MaxPlayerCount]{}; //!< Per-seat game-over observe freeze; zeroed in Clear.
		// The player death sceneman scroll targets, for when a player-controlled actor dies and the view should go to his last position
		Vector m_DeathViewTarget[Players::MaxPlayerCount];
		// The actor a spectating player's view follows; local presentation, so it stays out of checkpoints
		Actor* m_SpectatorTarget[Players::MaxPlayerCount];
		// Times the delay between regular actor swtich, and going into manual siwtch mode
		Timer m_ActorSelectTimer[Players::MaxPlayerCount];
		// The cursor for selecting new Actors
		Vector m_ActorCursor[Players::MaxPlayerCount];
		// Highlighted actor while cursor switching; will be switched to if switch button is released now
		Actor* m_pLastMarkedActor[Players::MaxPlayerCount];
		// The last selected landing zone
		Vector m_LandingZone[Players::MaxPlayerCount];
		// Whether the last craft was set to return or not after delivering
		bool m_AIReturnCraft[Players::MaxPlayerCount];
		// Icon Y offset the LZ handler hands the next queued delivery for multi-order stacking
		float m_NextMultiOrderYOffset[Players::MaxPlayerCount];
		std::array<std::unique_ptr<PieMenu>, Players::MaxPlayerCount> m_StrategicModePieMenu; //!< The strategic mode PieMenus for each Player.
		// The inventory menu gui for each player
		InventoryMenuGUI* m_InventoryMenuGUI[Players::MaxPlayerCount];
		// The in-game buy GUIs for each player
		BuyMenuGUI* m_pBuyGUI[Players::MaxPlayerCount];
		// The in-game scene editor GUI for each player
		SceneEditorGUI* m_pEditorGUI[Players::MaxPlayerCount];
		bool m_LuaLockActor[Players::MaxPlayerCount]; //!< Whether or not to lock input for each player while lua has control.
		Controller::InputMode m_LuaLockActorMode[Players::MaxPlayerCount]; //!< The input mode to lock to while lua has control.
		// The in-game important message banners for each player
		GUIBanner* m_pBannerRed[Players::MaxPlayerCount];
		GUIBanner* m_pBannerYellow[Players::MaxPlayerCount];
		// What a script gets for a seat this machine does not present: an inert object of the same type instead of a
		// nil it would have to check. They are never created, so every call is a no-op and every getter answers neutral.
		mutable std::unique_ptr<BuyMenuGUI> m_SeatStubBuyGUI[Players::MaxPlayerCount];
		mutable std::unique_ptr<SceneEditorGUI> m_SeatStubEditorGUI[Players::MaxPlayerCount];
		mutable std::unique_ptr<GUIBanner> m_SeatStubBanner[2][Players::MaxPlayerCount];
		// How many times a banner has been repeated.. so we dont' annoy by repeating forever
		int m_BannerRepeats[Players::MaxPlayerCount];
		// Whether each player has marked himself as ready to start. Can still edit while this is set, but when all are set, the game starts
		bool m_ReadyToStart[Players::MaxPlayerCount];
		// An override purchase list that can be set by a script and will be used instead of what's in the buy menu. Object held in here are NOT OWNED
		// Once a delivery is made with anything in here, this list is automatically cleared out, and the next delivery will be what's set in the buy menu.
		std::list<const SceneObject*> m_PurchaseOverride[Players::MaxPlayerCount];

		// The delivery queue which contains all the info about all the made orders currently in transit to delivery
		std::deque<Delivery> m_Deliveries[Teams::MaxTeamCount];
		// The box within where landing zones can be put
		Scene::Area m_LandingZoneArea[Teams::MaxTeamCount];
		// What a team outside the roster lands within: nowhere
		inline static const Scene::Area s_NoLandingZone;
		// How wide around the brain the automatic LZ is following
		int m_BrainLZWidth[Players::MaxPlayerCount];
		// The objective points for each team
		std::list<ObjectivePoint> m_Objectives;

		// Tech of player
		std::string m_TeamTech[Teams::MaxTeamCount];
		bool m_TeamTechSwitchEnabled[Teams::MaxTeamCount];

		// Initial gold amount selected by player in scenario setup dialog
		int m_StartingGold;
		// Whether fog of war was enabled or not in scenario setup dialog
		bool m_FogOfWarEnabled;
		// Whether we need a clear path to orbit to place brain
		bool m_RequireClearPathToOrbit;

		// Default fog of war switch state for this activity, default -1 (unspecified)
		int m_DefaultFogOfWar;
		// Default clear path to orbit switch value, default -1 (unspecified)
		int m_DefaultRequireClearPathToOrbit;
		// Default deploy units swutch value, default -1 (unspecified)
		int m_DefaultDeployUnits;
		// Default gold amount for different difficulties, defalt -1 (unspecified)
		int m_DefaultGoldCakeDifficulty;
		int m_DefaultGoldEasyDifficulty;
		int m_DefaultGoldMediumDifficulty;
		int m_DefaultGoldHardDifficulty;
		int m_DefaultGoldNutsDifficulty;
		int m_DefaultGoldMaxDifficulty;
		// Whether those switches are enabled or disabled in scenario setup dialog, true by default
		bool m_FogOfWarSwitchEnabled;
		bool m_DeployUnitsSwitchEnabled;
		bool m_GoldSwitchEnabled;
		bool m_RequireClearPathToOrbitSwitchEnabled;
		bool m_BuyMenuEnabled;

		// The cursor animations for the LZ indicators
		std::vector<BITMAP*> m_aLZCursor[4];
		std::array<int, Players::MaxPlayerCount> m_LZCursorWidth; //!< The width of each players' LZ cursor.
		// The cursor animations for the objective indications
		std::vector<BITMAP*> m_aObjCursor[4];

		// Time it takes for a delivery to be made, in ms
		long m_DeliveryDelay;
		// Cursor animation timer
		Timer m_CursorTimer;
		// Total gameplay timer, not including editing phases
		Timer m_GameTimer;
		// Game end timer
		Timer m_GameOverTimer;
		// Time between game over and reset
		long m_GameOverPeriod;
		// The winning team number, when the game is over
		int m_WinnerTeam;

		std::string m_NetworkPlayerNames[Players::MaxPlayerCount];

		/// Private member variable and method declarations
	private:
		/// The peer that drives a seat in the agreed roster, or 0 when no peer holds it.
		static uint8_t LockstepSeatPeerId(int player);
		/// The seat holder's display name from the agreed roster, for the match's own banners.
		static std::string LockstepSeatName(int player);
		/// Makes every brain already standing in the scene its seat's resident, identically on every peer, so
		/// a local editor's residence test can never take an actor out of one peer's sim alone.
		void SeedLockstepResidentBrains();
		/// True while the match's setup editor is the synchronized one: placements cross the wire.
		static bool IsLockstepPlacement();
		/// Whether this peer is the one that commits a seat's brain placement.
		bool MayCommitBrainPlacement(int player) const;
		/// Puts one seat's committed placement on the wire. `via` names the path that read the spot.
		bool CommitLockstepBrainPlacement(int player, const std::string& className, const std::string& preset, const std::string& module, const Vector& spot, const char* via);
		/// The ground under a scene x, where a brain settles under the same physics on every peer.
		Vector GroundSpot(float sceneX) const;
		/// Puts a refused placement where the player who tried can see it: their own screen for a few seconds,
		/// the console, and a banner when this peer is the one that was refused.
		void RefuseBrainPlacement(int player, const std::string& reason, bool banner);
		/// Runs the seat's queued scripted editor gesture, if it has one, the way that seat's own input would.
		void DriveScriptedSetupEditor(int player);
		/// Runs a queued actor-select onto a craft passenger for a presented seat after the match starts.
		void DriveScriptedActorSelect(int player);
		/// Draw-only ActorSelect/Go-To pie highlight for a local seat.
		void ApplyCursorHighlightDraw(int player);
		/// Clears the draw-only highlight when the cursor leaves an actor or those views.
		void ClearCursorHighlightDraw(int player);
		Actor* m_pLastHighlightDrawActor[Players::MaxPlayerCount]; //!< Draw-only last ring target; never dumped.
		/// Builds every seat's committed brain, in seat order, from a unique-id counter pinned to the same
		/// value on every peer. A local editor's own preview objects take ids off that counter on one peer
		/// alone, so the shared brains are made only after it is put back in step.
		/// @return Whether every seat's brain was built.
		bool BuildLockstepSeatBrains();
		static constexpr long c_SetupEditorUidReserve = 65536; //!< Ids a peer's own setup editor may spend before the shared ones resume.
		std::array<bool, Players::MaxPlayerCount> m_LockstepPlacementSubmitted{}; //!< Per-seat, local only: this peer has committed that seat's placement.
		std::array<NetGamePlaceBrain, Players::MaxPlayerCount> m_LockstepSeatBrains{}; //!< Per-seat committed placement; player < 0 means none yet.
		long m_LockstepPlacementUidBase = 0; //!< The unique-id counter as the editing phase opened, identical on every peer.
		bool m_LockstepPlacementSeeded = false; //!< The one-time seed pass has run for this editing phase.

		bool LoadNetLocalGameState(std::string_view text);
		bool CreateNetLocalUI();
		/// Points a relaunch's pending marked-actor links at the marks as they stand, so its deferred rebinds keep them.
		void RefreshCheckpointMarkedActorIDs();
		std::string SaveValueCheckpoint() const;
		bool LoadValueCheckpoint(std::string_view text, bool validateOnly = false);
		std::array<long, Players::MaxPlayerCount> m_CheckpointMarkedActorIDs{};
		bool m_HasCheckpointMarkedActorIDs = false;

		template <class Archive, class Self> static void VisitCheckpoint(Archive& archive, Self& self) {
			archive(self.m_CPUTeam, self.m_TeamIsCPU, self.m_ObservationTarget, self.m_DeathViewTarget,
				self.m_ActorSelectTimer, self.m_ActorCursor, self.m_LandingZone, self.m_AIReturnCraft,
				self.m_NextMultiOrderYOffset, self.m_LuaLockActor, self.m_LuaLockActorMode, self.m_BannerRepeats,
				self.m_ReadyToStart, self.m_LandingZoneArea, self.m_BrainLZWidth, self.m_Objectives,
				self.m_TeamTech, self.m_TeamTechSwitchEnabled, self.m_StartingGold, self.m_FogOfWarEnabled,
				self.m_RequireClearPathToOrbit, self.m_DefaultFogOfWar, self.m_DefaultRequireClearPathToOrbit, self.m_DefaultDeployUnits,
				self.m_DefaultGoldCakeDifficulty, self.m_DefaultGoldEasyDifficulty, self.m_DefaultGoldMediumDifficulty, self.m_DefaultGoldHardDifficulty,
				self.m_DefaultGoldNutsDifficulty, self.m_DefaultGoldMaxDifficulty, self.m_FogOfWarSwitchEnabled, self.m_DeployUnitsSwitchEnabled,
				self.m_GoldSwitchEnabled, self.m_RequireClearPathToOrbitSwitchEnabled, self.m_BuyMenuEnabled, self.m_LZCursorWidth,
				self.m_DeliveryDelay, self.m_CursorTimer, self.m_GameTimer, self.m_GameOverTimer,
				self.m_GameOverPeriod, self.m_WinnerTeam, self.m_NetworkPlayerNames);
			// The synchronized setup editor's state is shared state: a resync taken while the seats are
			// still placing has to restore the same placements and the same id base on every peer.
			archive(self.m_LockstepPlacementUidBase, self.m_LockstepPlacementSeeded);
			for (auto& placement: self.m_LockstepSeatBrains) {
				archive(placement.team, placement.player, placement.posX, placement.posY,
					placement.className, placement.preset, placement.module);
			}
		}
		/// Clears all the member variables of this Activity, effectively
		/// resetting the members of this abstraction level only.
		void Clear();
	};

} // namespace RTE
