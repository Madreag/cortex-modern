#pragma once

#include "ControllerFrame.h"
#include "NetLockstep.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace RTE {
	/// CLI scenario direct-launch mode.
	///
	/// Activated when the binary is invoked with `-scenario <PresetName>`. Skips the menu loop,
	/// starts the named activity directly via `g_ActivityMan.StartActivity`, runs the normal
	/// `RunGameLoop` — visible by default so the user can *watch*, or hidden under `-headless` /
	/// `-tick-hashes` for an automated run (`-headed` forces a window back on) — and exits when
	/// the activity ends or the `-max-ticks` cap is hit. The `MetricsCollector`'s JSON report is
	/// written if `-out <path>` was supplied.
	///
	/// Used both for "run a scenario and watch it" workflows and for batch determinism runs
	/// driven headless by the `-determinism-check` orchestrator.
	class ScenarioRunner {
	public:
		struct Args {
			std::string testScript; // -test-script <lua>: attached to every actor entering the world (a mod-style script for the restore gates).
			std::string scenario;     // single PresetName-suffix, e.g. "SimBaseline". One scenario per
			                          // invocation — batching is a shell-script concern
			                          // (FMOD/module-reload across activities in one process is messy).
			std::string outPath;      // JSON output path; empty = no report
			uint64_t    seed = 0;     // deterministic seed; 0 = use SeedRNG()'s default
			uint64_t    maxTicks = 0; // 0 = scenario-default safety cap (1800 ticks / 30 sim seconds)
			int         numLuaStates = -1; // -num-lua-states override, if supplied
			std::string controllerLogOutPath; // -controller-log-out path
			std::string controllerLogInPath; // -controller-log-in path
			bool        controllerLogCanonicalize = true; // record sim uses encoded->decoded frames
			bool        controllerReplayStrict = false; // strict replay skips local Controller/AI production
			bool        controllerLogAllowNlsMismatch = false; // cross-nls replay stress only
			std::string controllerDebugDumpPath; // -controller-debug-dump JSONL path for replay diagnostics
			std::vector<std::pair<uint64_t, uint64_t>> controllerDebugDumpTicks; // optional inclusive tick ranges
			bool        tickHashes = false; // -tick-hashes: emit per-tick hash trace into the JSON
			                                 // report, read by the determinism check to diff
			                                 // multiple runs of the same scenario+seed.
			bool        selftestPerturb = false; // -determinism-selftest-perturb: inject one genuine
			                                 // non-determinism at a fixed tick (the determinism
			                                 // check's positive control).
			bool        selftestFundsCommand = false; // -net-match-e2e-funds-command: host-issued funds command at tick 50.
			bool        selftestSpawnCommand = false; // -net-match-e2e-spawn-command: host-issued spawn command at tick 50.
			bool        selftestDeliverCommand = false; // -net-match-e2e-deliver-command: host-issued delivery command at tick 50.
			bool        selftestAIOrderCommand = false; // -net-match-e2e-ai-order-command: the host orders its units (go-to, follow, squad, disband) at ticks 50/200/400/600.
			bool        selftestScuttleCommand = false; // -net-match-e2e-scuttle-command: host scuttles the delivered craft at tick 100.
			bool        selftestBrainKillCommand = false; // -net-match-e2e-brain-kill-command: host delivers + scuttles a craft onto the enemy brain.
			bool        selftestStall = false; // -net-match-e2e-stall: this peer stops producing frames for 8s at tick 300 (stall-grace test).
			bool        selftestJoinRejection = false; // -net-match-e2e-join-rejection: stop the harness after recording a refused join.
			bool        scriptGraphSelfTest = false; // -script-graph-selftest: run the script graph's contract tests in the master state and exit.
			bool        freeRunSim = false; // -free-run-sim: a lockstep match runs its ticks as fast as the frames arrive, one per loop iteration, drawing nothing.
			bool        selftestRematch = false; // -net-match-e2e-rematch: when match 1 ends, return to the lobby and run a second match.
			bool        selftestLeave = false; // -net-match-e2e-leave: this peer quits to the menu at tick 300 like a pause-menu leave.
			bool        selftestInventoryCommand = false; // -net-match-e2e-inventory-command: host-issued inventory ops at fixed ticks.
			bool        selftestBuyCommand = false; // -net-match-e2e-buy-command: host grants funds then places a real buy order through CreateDelivery.
			bool        selftestBrainSpawnCommand = false; // -net-match-e2e-brain-spawn-command: this peer spawns a second brain for its team at tick 40.
			bool        selftestPauseCommand = false; // -net-match-e2e-pause-command: host pauses at tick 250 and unpauses at 430.
			bool        selftestSnapshot = false; // -net-match-e2e-snapshot: save the full game at tick 300 (both peers save the same synced frame).
			uint64_t    contractAuditContinueThrough = 0; // Diagnostic-only: keep running after the audit operation to this complete tick.
			long long   contractAuditSeedMarker = 0; // Diagnostic fixture value placed in all Lua roots before a seed is saved.
			bool        contractAuditContinuationPerturb = false; // Positive control: one extra simulation RNG draw after the observed operation.
		};

		/// True if `-scenario` was supplied on the command line.
		static bool IsActive();

		/// Set by ParseArgs. Reset for tests.
		static void SetActive(bool active);

		/// Parsed args. Only valid when IsActive() is true.
		static const Args& GetArgs();

		/// Parse a single CLI flag starting at startIndex. Returns the number of argv items
		/// consumed (0 / 1 / 2). Sets active = true when `-scenario` is seen.
		static int ParseArgs(int argCount, char** argValue, int startIndex);

		/// Diagnostic-only ordinary-load and staged-candidate transactions. Calls the production
		/// entry points directly; the observer records both the live world and pending candidate.
		/// A handled operation reports preparation/application separately. It never repairs state.
		static bool RunContractAuditLoad(const std::string& operation, const std::function<void(const std::string&)>& observe,
		                                bool& prepared, bool& applied);
		static void SetContractAuditSeedMarker();
		static void PerturbContractAuditContinuation();

		/// Build the full preset name from the `-scenario` arg (prefixes with "Determinism ").
		/// Returns "Determinism SimBaseline" for arg "SimBaseline", or the raw string if already prefixed.
		static std::string ResolvePresetName(const std::string& shorthand);

		/// After the activity ends and RunGameLoop returns, write the metrics report (if -out set).
		/// Returns exit code: 0 if scenario passed (or no result was set), 1 otherwise.
		static int FinalizeAndGetExitCode();

		/// Prepare ControllerFrame record/replay state after deterministic config is pinned.
		static bool PrepareControllerLog(std::string* error = nullptr);

		static bool IsControllerLogRecording();
		static bool IsControllerLogReplaying();
		static bool ShouldCanonicalizeControllerLog();
		static bool IsControllerReplayStrict();
		static bool IsControllerDebugDumpEnabled();
		static bool ShouldControllerDebugDumpTick(uint64_t tick);
		static const std::string& GetControllerDebugDumpPath();
		static void RecordControllerFrames(uint64_t tick, std::vector<ControllerFrame> frames);
		static bool GetReplayControllerFrames(uint64_t tick, std::vector<ControllerFrame>& outFrames, std::string* error = nullptr);
		static void SetControllerReplayError(const std::string& error);
		static void ClearControllerReplayError();
		static bool HasControllerReplayError();
		static const std::string& GetControllerReplayError();

		static void SetLockstepCoordinator(NetLockstepCoordinator* coordinator);
		/// Pumps the coordinator until the relay host owes no peer a forward, or the budget runs out,
		/// then keeps relaying for lingerMs. The star's hub is the only route between its clients, so
		/// quitting with a forward still held takes the round off every client that was waiting on it.
		/// Returns whether it drained.
		static bool DrainLockstepRelay(uint32_t budgetMs, uint32_t lingerMs);
		static bool FinishLockstepSimulationTick(uint64_t completedTick);

		/// Enable the "waiting for peer" overlay drawn while the lockstep wait is stalled. Interactive
		/// matches only — automated runs keep their output clean and have no visible window.
		static void SetLockstepStallOverlayEnabled(bool enabled);
		static bool IsLockstepControllerSyncActive();
		/// Whether a lockstep coordinator is attached at all — a FAILED one still owns the sim (the
		/// tick must surface its stop reason, never silently degrade to per-machine controllers).
		static bool HasLockstepCoordinator();
		/// The attached coordinator's stop reason ("" while running or when absent).
		static std::string GetLockstepStopReason();
		/// Accumulated wall time spent inside the lockstep frame wait, for the pace report.
		static long long GetLockstepWaitUs();
		/// Zeroes the lockstep wait accumulator, for per-round pace windows.
		static void ResetLockstepWaitUs();
		static bool IsLockstepLocalActor(int64_t actorUniqueID, int actorTeam, bool cpuControlled);
		/// Whether the peer produces the actor's frames (control handoffs included); every peer answers identically.
		static bool IsLockstepActorOwner(int64_t actorUniqueID, int actorTeam, bool cpuControlled, uint8_t peerId);
		/// The peer that produces the actor's frames now (control handoffs included); 0 without a coordinator.
		static uint8_t GetLockstepActorOwner(int64_t actorUniqueID, int actorTeam, bool cpuControlled);
		/// The synced match's host peer; 0 without a coordinator.
		static uint8_t GetLockstepHostPeerId();
		static uint8_t ResolveTeamCommandAuthority(int team);
		static bool SubmitLockstepChecksum(uint64_t tick, const std::array<uint8_t, 32>& hash);
		static uint16_t GetLockstepInputDelayFrames();
		static uint8_t GetLockstepLocalPeerId();

		/// Whether a team has a human player in the synced match config. Local player bindings are
		/// per-peer in a lockstep match, so sim decisions must resolve team humanity from here.
		static bool IsLockstepHumanTeam(int team);

		/// Whether any player slot (human or CPU) in the synced match config plays this team.
		static bool IsLockstepActiveTeam(int team);

		/// Whether the actor's owner peer has cleanly left as of the given frame; survivors stand the
		/// leaver's actors down at the identical tick because the lockstep gate syncs the knowledge.
		static bool IsLockstepActorOwnerGone(int64_t actorUniqueID, int actorTeam, bool cpuControlled, uint64_t frame);

		/// Whether the round is running only because a dropped player still has a seat to come back to.
		static bool IsLockstepHoldingSeatForReclaim();

		/// The session upkeep the match service owns. A peer that stops sending frames parks the sim
		/// thread in the lockstep wait, so without this the admission plane cannot answer anything -
		/// including the leave the waited-for peer is waiting to have acknowledged.
		static void SetSessionPump(std::function<void()> pump);

		/// Records a synced control handoff: the actor's frames now come from this peer. Co-op players
		/// share a team, so per-actor control must override the per-team ownership policy.
		static void SetLockstepControlOverride(int64_t actorUniqueID, uint8_t ownerPeerId);
		/// Drops handoffs held by peers that have left as of this frame, so their actors revert to the
		/// team's policy owner (a surviving teammate's AI picks them up). Synced: every peer consumes
		/// the frame with identical leave knowledge.
		static void PurgeLockstepControlOverridesForGonePeers(uint64_t frame);
		/// Whether this peer may issue team commands for the team (any of a shared team's human peers may).
		static bool IsLockstepTeamCommandSender(int team, uint8_t senderPeerId);
		/// The local peer's index among the team's human slots in the synced roster, or -1 outside a
		/// lockstep match / off the team. Per-peer view data — for per-peer picks only, never sim decisions.
		static int GetLockstepHumanSlotIndex(int team);

		/// Match replay: the recorder captures every committed tick (all peers' frames + commands)
		/// behind the synced config header; playback feeds them through the identical apply path.
		static void ArmLockstepReplayRecord(const std::string& path);
		static bool BeginLockstepReplayRecord(const NetMatchConfig& config, std::string* error = nullptr);
		static bool IsLockstepReplayRecording();
		static uint64_t GetLockstepReplayFramesWritten();
		static void CloseLockstepReplayRecord();
		static bool SetLockstepReplaySource(const std::string& path, std::string* error = nullptr);
		static bool IsLockstepReplayPlayback();
		static const NetMatchConfig& GetLockstepReplayConfig();
		static uint64_t GetLockstepReplayStartFrame();
		/// The rollback fidelity gate under playback: keep copies of the probe window's records
		/// during the first pass, then rewind the coordinator and re-feed them for the re-run.
		static void ArmReplayRewindBuffer(uint64_t fromFrame, uint64_t frameCount);
		static bool RewindReplayForProbe(uint64_t firstFrame, std::string* error = nullptr);

		/// How a playback ended. Set where the condition is detected, never inferred from message text.
		enum class LockstepReplayOutcome {
			None,
			Playing,
			Completed,
			TickCap,
			Truncated,
			Corrupt,
			SimFailure,
		};
		static LockstepReplayOutcome GetLockstepReplayOutcome();
		static void SetLockstepReplayOutcome(LockstepReplayOutcome outcome);
		static const char* ReplayOutcomeName(LockstepReplayOutcome outcome);
		static uint64_t GetLockstepReplayFramesConsumed();
		static uint64_t GetLockstepReplayLastTick();
		static bool LockstepReplaySawEndMarker();
		/// The last recording's frame count and whether it was closed with its end marker.
		static uint64_t GetLockstepReplayRecordFrames();
		static bool WasLockstepReplayRecordClosed();

		/// The synced lockstep pause: both sims stop after the same frame and resume together after a
		/// shared null-tick countdown, while the wire keeps exchanging empty frames.
		static bool IsLockstepPaused();
		static int GetLockstepResumeCountdown();
		static void ApplyLockstepPauseCommand(bool pause);
		static void AdvanceLockstepPausedTick();
		static bool QueueLockstepLocalControllerFrames(uint64_t tick, std::vector<ControllerFrame> frames, std::string* error = nullptr);
		static bool WaitForLockstepControllerFrame(uint64_t tick, NetLockstepReadyFrame& outFrame, std::string* error = nullptr);
		/// The local frames already queued for a future lockstep tick (the input-delay pipeline).
		static bool PeekLockstepLocalControllerFrames(uint64_t tick, std::vector<ControllerFrame>& outFrames);
		/// The local sender's input delay in ticks; 0 outside a delayed lockstep match.
		static uint16_t GetLockstepLocalInputDelay();

		/// Enqueue an owner-issued game command to ride the next local lockstep frame; the coordinator stamps
		/// the sender and both peers apply it at the synced frame.
		static void EnqueueLocalGameCommand(const NetGameCommand& command);
		static std::vector<NetGameCommand> DrainLocalGameCommands();

		/// Pin the sim-affecting config to canonical values for a deterministic run, so the sim is
		/// bit-identical across machines regardless of per-machine Settings.ini. Call after settings
		/// load, before the sim starts.
		static void ApplyDeterministicConfig();

		/// Snapshot the effective sim-affecting config as key=value strings, for the trace fingerprint.
		static std::map<std::string, std::string> GatherSimConfig();
	};

} // namespace RTE
