# Pie lockstep regression arms

These fixtures detect stale player input on AI actors and local pie-state
mutations. They do not repair the engine. Run `run_arm.py` through the existing
isolated runner with a fresh output directory and an already firewall-ruled
executable. It uses headless mode, separate peer runtimes, input delay 3, 320
ticks, and dumps ticks 27..320. It never builds or copies an executable.

```powershell
python tools/pie_lockstep/run_arm.py actor_cancel D:/mx/astra-pie-gating-20260913/example-net --exe '<retained executable>' --port 47922 --observe
python tools/pie_lockstep/run_arm.py actor_cancel D:/mx/astra-pie-gating-20260913/example-sp --exe '<retained executable>' --sp --observe
python tools/pie_lockstep/verify_pie_close.py actor_cancel D:/mx/astra-pie-gating-20260913/example-net --reference D:/mx/astra-pie-gating-20260913/example-sp --out result.json
```

Cases are `next`, `prev`, `goto`, `actor_cancel`, and `delivery_cancel`. The two
cancel fixtures enter their view using the public Lua API, then exercise native
secondary-button cancellation. They do not test the 250ms real-time hold for
ActorSelect entry, buying a delivery, or delivery confirmation.

The detector requires successful and complete `manifest.json`, `run_result.json`
and peer `launch.json` evidence, all actor/activity rows, every trace row and
subsystem, and full peer dump equality. It checks actual actor identities, the
replacement seat and consumed switch edge. AI player-only input is forbidden.
NEXT requires the departing pie to stay open until the committed handoff, then be
Disabled at every tick from handoff+4 through 320: the 50 ms disable animation
takes four sim ticks here and in SP, which is Disabling at its own handoff..+3.

Cancellation checks require `--reference`. They compare every pie field and
Lua observation from net tick 199 through 320 with SP ticks 196..317, including
the committed cancellation at 202. NEXT and PREV read the same reference through
the same input delay, net 204..320 against SP 201..317.
`compare_reference.py` calls this same detector. Delivery's mode-change callback count has a separate exact expectation:
two before the committed cancellation, three afterwards. Its direct SP
`SetInputMode` calls do not notify, while frame application does. Pie getters,
pie callbacks, controller mode and callback counts are all checked.

`compare_sp.py` compares full SP dumps, all trace rows and all Lua observation
rows between two executable versions. A matching repeat does not erase an
earlier failed pair or establish run-to-run stability. The observer records
end-of-update getters and callback counts; it does not verify getters inside
mode-change callbacks, scripted slice activation, or submenu timing.

```powershell
python tools/pie_lockstep/test_detector.py D:/mx/astra-pie-gating-20260913/red-next-debug D:/mx/astra-pie-gating-20260913/example-mutations --cancel-root D:/mx/astra-pie-gating-20260913
```

Mutation data are explicitly synthetic and contain synthetic successful runner
metadata. They are accepted only through the unit-test API's explicit opt-in;
the real-run CLI refuses them. The suite requires three positive baselines and
43 rejections, including the named handoff/activity mutations, isolated
SECONDARY_ACTION, shared post-cancel corruption, callback corruption and missing
or failed launch evidence. Synthetic success is not repaired-engine evidence.
