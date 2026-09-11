from pathlib import Path
from datetime import datetime, timezone
import html

root = Path('D:/Projects')
stamp = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')
summary = (
    f'{stamp} - RECOVERY HANDOFF after the Codex usage-limit stop (thread 01a07bf2-1ee0-7b63-86f8-21dc6ef8e3cc; root and its three subagents errored at 02:47:05 UTC with usage_limit_exceeded; nothing was recorded between 02:34 and the stop). '
    'Reconstructed from the thread\'s tool log and the disk. Main D:/Projects/p4b-interp-validation is unchanged at c8f8188ae0 (Source41, 138 ahead of origin, unpushed); approved executable bb3cf264b4e7 unchanged. '
    'The worker tree D:/Projects/takeover-fixes (stage2/takeover-next HEAD 60cb698146 = main + round-start 8f493e8ecf + B2 moderation 703e3f1202 + two composition commits) still carries the whole uncommitted group (72 modified/16 untracked files vs main, +7991/-345, never committed, never built on Windows); a non-destructive snapshot (patch, untracked tarball, hash manifest, README) is in reviews/takeover-20260909/handoff-20260910/. '
    'Its verified state is positive20 (Mac manifest 29938cd8ba99, GNS binary 238066fa01d9): ten network CLI selftests, controller-frame, native-graph with diagnostics off and on, the same on the no-GNS build (native-positive20-nogns-verified-0243, copied back today, 81 files hash-verified), and, finished at 02:56 UTC after the stop and unrecorded until now, THE FULL A7 GROUP PASSES ON POSITIVE20: stagger_seat_survives (902 matched frames), silent_socket_bound (12,002), leave_ack_dropped (300), slow_resync_save (12,002), coop_hand_back (12,002), source unchanged before and after every arm (a7-verified-mac-group-positive20-0256, 283 files hash-verified). '
    'The 02:34 red (craft-fixture duplicate-ID Console errors) is therefore closed by positive20\'s LuaMan.cpp fixture isolation (craft-fixture-isolation-20260910/fix.patch). '
    'On top of positive20 the tree holds about 650 lines of UNBUILT A7/B2 journal and observer instrumentation (new NetRecoveryJournal.h/.cpp, NetA7Journal helpers, hooks in NetLockstep.cpp, host observations in NetReconnectSession.*, NetSession.*, NetMatchService.cpp, NetReconnectTxCache.*, NetLobbySession.cpp, vcxproj/filters/meson entries) that root and the adversarial reviewer were mid-edit on when the limit hit (last write 02:47:29 UTC); it is isolated as handoff-20260910/post-positive20.patch and has never been compiled: a draft to review, not part of positive20. '
    'The Windows Source41 breadth v3 (D:/mx/s41b3, unchanged exe) finished on its own at 02:52 UTC: 81/81 collected, 68 pass, 13 non-pass, UNREVIEWED: fl200 (auto delay differs from 14 on both peers; Source40 gave 14), h4_clean_leave_1, h4_reclaim_socket_1, h4_rejoin_after_resync_1 (host exit 1 with "Rejected a AIOrder command from a peer that does not control team 1"), h4_fencing_two_transports_1 (client2 setup: connection closed by remote host), h4_peers_3_4_regression_3 (audio checkpoint restoration failure on the returner, the audio-flake class; the other four repetitions pass where Source40 was 0/5), b1_substitute_commit and b1_substitute_returner_wins (host and stayer exit 1, no seat drop evidenced), heal (harness: ModuleNotFoundError recovery_e2e in the breadth wrapper, not an engine result), compat_deferral_source22/approved (output differs from Source22, no UID exclusions applied), compat_extra_source22/approved (the fixture calls SoundSet:HasAnySounds and fails identically on both rungs: a fixture defect). '
    'The four H4 gates were already red with known residues on Source38-40 (lanes/source33-lanes/report.md, section Source40); whether the AIOrder rejection is a new Source41 symptom of the transport-authority commit 9b84ddc7df is open. '
    'Matrix: the continuation review (source41-matrix-review/continuation-20260910/review.md) passes 42/44 new jobs; heal_global_d0/d3 stay OPEN_SHARED_STATE_RESIDUAL (host-only AI_StuckForTime NumberValue on UID 1048653, written by SharedBehaviors.lua:547, consumed by the stock Automovers controller); mp_snapshot_p5 stays failed (the comparator handles only Activity1 and picks the brain, not the controlled actor; the narrow tools/compare_snapshots.py repair passes 61 controls and the retained pair but lives only in takeover-fixes); peer_roundtrip stays blocked. '
    'Other frozen but unexecuted subagent output: the B2 live-driver package (b2-live-group-20260910/CONTRACT_V2.md, continuity_oracle.py, live_group.py, recovery_codec.py) and the observer plan (b2-review/b2-native-observer-plan-20260910.md, lobby-transfer-hardening-20260910.md). '
    'No Windows or Mac engine, build or wrapper is running; the Mac has 39 GB free (91% used), D: has 126 GB free. No merge, commit, push, cleanup or milestone acceptance. '
    'Delegation policy changed by the user today: workers are Cursor subagents on Grok 4.6 (xhigh reasoning, fast mode), briefed narrowly, reviewed and corrected by the lead; see AGENTS.md.'
)
path = root / 'RESUME.md'
value = path.read_text(encoding='utf-8')
heading = '# §B. WHERE WE ARE — START HERE\n'
assert value.count(heading) == 1 and 'RECOVERY HANDOFF after the Codex usage-limit stop' not in value
path.write_text(value.replace(heading, heading + '\n**' + summary + '**\n', 1), encoding='utf-8')
short = (f'**{stamp}: recovery handoff after the 02:47 UTC usage-limit stop.** Positive20 passes the full Mac A7 group (five arms) plus the native/no-GNS gates; the working tree also holds about 650 lines of unbuilt journal/observer instrumentation (handoff-20260910/post-positive20.patch). '
         'Source41 Windows breadth finished 68/81 with 13 unreviewed non-pass cases. Main c8f8188ae0 and exe bb3cf264 unchanged, unpushed. See RESUME.md §B and reviews/takeover-20260909/handoff-20260910/README.md.\n\n')
for relative in ('STAGE2_H4_RECONNECT_PLAN.md', 'reviews/recovery-2026-09-07/contract-audit/CONTRACTS.md', 'reviews/recovery-2026-09-07/contract-audit/mac-peer-20260907/MAC_RESUME.md', 'reviews/claude-review-2026-09-08/INTEGRATION_33_RUNBOOK.md'):
    path = root / relative
    path.write_text(short + path.read_text(encoding='utf-8'), encoding='utf-8')
path = root / 'reviews/takeover-20260909/b2-mac-verification.md'
path.write_text('**' + summary + '**\n\n' + path.read_text(encoding='utf-8'), encoding='utf-8')
path = root / 'PRs/PR_ROADMAP.html'
value = path.read_text(encoding='utf-8')
assert value.count('<body>') == 1
path.write_text(value.replace('<body>', '<body>\n<p>' + html.escape(summary) + '</p>', 1), encoding='utf-8')
print(stamp + ': all live pointers updated')
