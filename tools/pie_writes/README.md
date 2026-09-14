# F23c local pie-write arms

Three two-peer lockstep arms for the local UI writes in `Source/Activities/GameActivity.cpp` that F23's
gating did not reach. They measure; they do not repair the engine.

- `buy_menu` — site A, `RemovePieSlicesByType(PieSliceType::BuyMenu)` when the activity's buy menu is
  off and the seat's pie is enabling. `PieWriteObserver.lua` turns `BuyMenuEnabled` off on every peer,
  so the condition itself is shared state.
- `form_squad` — site B, the FormSquad non-commander branch: `SetDisabled(true)` on the actor's
  controller and `SetEnabled(false)` on its pie menu.
- `full_inventory` — site C, `SetEnabled(false)` on the pie menu when FullInventory is picked.

Both slice cases activate through `FIRE` (PRESS_PRIMARY) while `PIEMENU_DIGITAL` is still held, because
releasing the pie button closes the menu on every peer through the controller and would hide the local
write. `L_DOWN`/`L_LEFT` hover a quadrant's middle slice and the counter-clockwise direction steps to the
second slice in that quadrant.

```powershell
python tools/pie_writes/run_write_arm.py full_inventory D:/mx/opus-f23c-20260913/<name> --exe '<retained executable>' --port 48181
python tools/pie_writes/verify_peer_pie.py full_inventory D:/mx/opus-f23c-20260913/<name> --out D:/mx/opus-f23c-20260913/score/<name>.json
```

The runner uses the existing isolated runner (`tools/run_sim_test.py` `make_run`), headless, separate peer
runtimes, input delay 3, 320 ticks, dumps 27..320, ports 48181-48189. It never builds or copies an
executable and refuses one with no inbound firewall rule.

The detector requires complete `manifest.json`, `run_result.json` and per-peer `launch.json` evidence, a
complete dump for every actor over ticks 27..320, and then compares the two peers: `pie`, `ctrl`, `mode`
and `dis` for every actor and tick, all 320 tick hashes with their subsystems, and the raw dumps line for
line (a difference is reported with the differing field names, never excluded). Each case also proves its
site was reached — the buy menu was switched off on both peers and some peer dropped a slice, or the named
slice was activated on the host — so a run that never opened the menu cannot pass.

Not covered: analog pie input, sub-pie menus, the BuyMenu slice on a human or a craft, a commander's
FormSquad (DisbandSquad), leaving UnitSelectCircle or the inventory GUI again, gamepad seats, more than
two peers, reconnect, and the Mac.
