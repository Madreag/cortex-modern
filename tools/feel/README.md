# Unattended feel measurements

Run only after build authorization and removal of the phase lock:

```powershell
$env:CCCP_HEADLESS = '1'
python -B tools/feel_measure.py --out D:\mx\feel-item9a-review --skip-gates
```

The driver uses the assigned worktree's executable and `run_sim_test.make_run` for
every engine process. Each process has its own private hidden desktop and writable
runtime. The matrix is service e2e at 100 and 200 ms fake lag in **both processes**,
automatic delay, prediction on, and 60 Hz or uncapped rendering. It deliberately
omits `-free-run-sim`: at the pinned source tip that option skips the normal draw
call. The ordinary e2e loop renders and contributes to the existing pace counters.
All traces request 1200 ticks. The service's stop/drain tail is retained separately
in the raw records; trace coverage must be exactly ticks 1 through 1200.

The item 9a cases add 5% GNS packet loss and a three-peer match whose client stops
at tick 600 for 1500 ms, then takes the normal reclaim path. The latter keeps AI
enabled and compares every committed hash on the two survivors through the hold
and rejoin. The impaired client uses twice the lag argument while the host and
survivor use zero, keeping 200 ms on each leg of that client's link. The loss arm
sets GNS send and receive loss in that client and records whether both setters
succeeded. It is reapplied when recovery opens another round.

Fast peers must sustain at least 59.5 ticks/s after tick 300, spend less than 1%
of that wall time waiting for input, and have no wait over 50 ms. The elapsed
window spans recovery; restarted pace counters cannot remove a rejoin pause.
The requested zero `steady_missing_frame_stalls` check is also retained. That
counter keeps its transport/prefetch meaning; actual blocking has a separate
counter and wait log. Every MISS is retained, including a disagreement between
those two measures. The slow client's presentation checks still run.

Delay checks read the engine's timestep and measured RTT, require the initial
pick to cover `ceil(RTT / tick) + 1`, and compare the final displayed delay with
the live configuration. The preview's 2 ms budget applies at the full negotiated
depth, including 25 or more ticks. None of these additions has been run in the
compile-only work phase.

There is one local SP baseline per render cap. It wraps the stock P4 Alpha Duel
activity, keeps its scene and loadout, seats two humans on teams 0 and 1, and
clears every CPU team (no funds, no brains). Human brains are parked out of
Battle Rifle reach so the 1200-tick window cannot be decided. It uses the same
input script and records the controller log. Network runs launch that same
fixture with `-net-match-humans 2 -net-match-cpu-slots 0`. The input seam is
the same one used by `record_ak47_fire.py`; no desktop input is synthesized.
Each three-tick firing pulse shares its press and release with a distinct aim
change, so those input packets have an operationally defined render-pose probe.
The generated input-schedule.json requires every planned edge stamp to be present.

`EnableVSync=0` is written to the private runtime's Settings.ini. This tip has no
numeric frame-cap property. A harness-only `FeelRender.ini`, passed using
`-feel-render-settings`, contains `RenderCapHz = 60` or `RenderCapHz = 0`.
Only the render path reads that cap. The setting is also present in the runs with
measurement recording off, so the proof compares the same presentation cap.

`-feel-measure <existing fresh directory>` enables the recorder; both harness flags
require `CCCP_HEADLESS=1`. They are off by default. New state resides in FrameMan's
file-local recorder, outside all controller, snapshot, dump and hash structures.
Controller.cpp changes only to stamp the wall clock immediately before the real
scripted input sampler; it queues the observation for writing on the render path.
No simulation decision reads a measurement, render cap, timestamp or output path.

Raw measurements are in each peer's `feel/raw.jsonl`:

- `input`: input edge, wall time, actor, delay and last presented frame.
- `preview`: local clone state at each predicted target tick and a wall interval
  around its existing update stages. No clone or resident is modified to measure it.
- `committed`: local actor copy at the next render boundary, before substitution.
- `frame`: the interpolated actor position, aim, velocity and controller copy used
  for drawing, draw duration, upload/present duration, frame interval, process CPU,
  preview counters, announced D and RTT snapshot.
- `iteration`: raw cumulative counters used by the existing service pace report,
  plus independent wall and process CPU counters.
- `capture`: requested tick, actual rendered tick, PNG path, save result and cost.
- `end`: orderly recorder termination and final process CPU time.

Definitions are fixed before measurements:

- Input-to-photon means the first completed `UploadFrame` whose actor render copy
  moves its aim closer to the scripted aim or changes velocity in the requested
  direction; the render controller must also carry the requested input. Movement releases
  require the movement bit cleared and speed reduced. A later input edge cannot
  supply an earlier edge's reflection. Unreflected edges retain a right-censored
  latency lower bound and remain MISS, never zero. Missing input stamps are an
  instrumentation-coverage failure; a recorded input with no response is a finding.
  The software presentation boundary includes upload, swap and frame housekeeping;
  a hidden desktop supplies no physical display scanout measurement. At 60 Hz the
  bound stays 34 ms. Uncapped uses one 60 Hz sim tick plus that presented frame's
  measured interval; the independent 50 ms frame gate remains unchanged.
- Latency distributions report milliseconds and actual presented-frame counts,
  with nearest-rank p50/p95/max. Draw and present report p99/max as well. The 50 ms
  gate checks inter-present intervals as well as draw and upload/present durations.
- CPU cost uses `GetProcessTimes` kernel plus user time from the first game-loop
  iteration to the final record, including the stop/drain tail, against the same-cap
  SP baseline. It is process CPU **time**, not a task-manager utilization sample.
  Recording and PNG costs are included; no overhead is subtracted.
- Corrections compare the latest recorded prediction for a target tick with the
  same actor's existing `CC_SIM_DUMP=1:1200` row, after late global callbacks.
  This provides an exact committed position even when several sim ticks fall
  between rendered frames. No new measurement field enters that dump. The raw
  render-boundary canonical copy and controller debug records are also kept. Missing canonical comparisons are
  incomplete measurements. Displacements greater than 4 px and both rolling 10 s
  wall and sim windows are reported. The replay's same-frame remote commands are
  listed as candidate causes. Coincidence does not prove cause: any unproven large
  correction keeps the remote-only requirement at MISS.
- Warps retain every nonzero residual between rendered displacement and trapezoidal
  velocity integration, using the rendered sim horizon plus interpolation alpha,
  20 pixels/metre, and wrapped scene distance. The output is a candidate trace, not
  a claim that acceleration or a contact is a defect; every candidate is retained.
- Audio uses the existing full `CC_TRACE_PREVIEW_EVENT` voice log. Each matching
  preview step supplies a conservative wall interval for the voice start; no mixer
  timestamp or audible sample is invented. Duplicate voice identities within the
  ledger's one-tick retiming window fail the once check. Both sound and firing must
  occur on the input's preview tick and within 34 ms. Voice output stays muted.
  The original ledger keeps only 64 detailed event starts; the full voice log and
  per-step records remain available after that capacity is reached.
- PNG capture uses this lane's `-feel-measure` seam, since the pinned source does
  not contain `-net-match-screenshot-ticks`. Files are named by requested tick,
  every 60 ticks; the raw row also records the actual presented tick. Both peers
  retain captures, including the required client captures.

Each instrumented pair has a fresh otherwise equivalent pair with recording off.
`hash-proof.json` runs the existing comparator unchanged and separately requires
the **entire** tick-hash rows to match, including totals and controller hashes.
Both peers with recording on, both peers with recording off, and each peer's
on/off run are compared. Auto-delay changes between runs are findings; the driver
does not impose a floor to force the pinned D or weaken a comparator.

The default gates are every entry in `run_selftests.SELFTESTS`, the script graph selftest with
four Lua states, and the SP pie-close control comparison using the unchanged
comparator recorded in the driver. The SP fixture and Index.ini are copied as
individual files from the retained control runtime; no directory tree is copied.
The gate's launch has neither new flag, verifying the default-off path.
`--sp-control` selects a retained successful pie-close control if the default
reference is unavailable. `--skip-gates` explicitly leaves the lane unverified.

`feel-report.json` and `summary.md` are written per measured pair, with a matrix
summary at the output root. The raw-file manifest includes file sizes and SHA-256.
A numerical MISS is a result. Missing evidence, a failed launch, failed hash proof
or missing gate prevents completion. Use a fresh child `--out` for a new run;
existing run trees are never deleted, moved or replaced. `--analyze-only` is for a
completed launch matrix whose analysis directories do not yet exist.

Detector checks are `python -B tools/feel/test_report.py`. They use synthetic
records only and do not establish engine correctness. They remain unrun in the
compile-only lane; the review runs them with the engine gates and drivers.
