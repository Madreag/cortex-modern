# W8: which SoundContainer owner identity a match save does not carry

Write-only under this directory. Phase 2 worktree `D:\Projects\audio-owner-registry` (`stage2/audio-owner-registry` @ `9751a90e29`) was not edited. Approved tree not touched. No build. No engine launch.

Commands: `python find_saves.py`, `python check_save.py <save> --json-out …`, `python run_priority.py`, `python run_phase1.py`, `python summarize_phase1.py`, `python clue_identity.py <save> 9310`. Proof files: `save_inventory.json`, `save_inventory.txt`, `phase1_results.json`, `phase1_summary.txt`, `phase1_compact.txt`, `phase1_priority.txt`, `validate_one.json`, `validate_p5resync.json`, `validate_source35.json`, `clue_9310.json`.

---

## Phase 1 table

Walk: `os.scandir`, skip symlink / `st_file_attributes & 0x400`, skip `Data` / `external` / `modules`. Roots `D:\mx` and `D:\Projects\reviews`.

| | |
|---|---|
| files seen | 115597 |
| reparse skipped | 4119 |
| skipdir | 113 |
| walk errors | 0 |
| `.ccsave` + unpacked `Save.ini` | 1847 (`save_inventory.txt`) |
| unique contents analyzed | 623 |
| saves whose AudioRuntime parsed with ≥1 voice | 1662 |
| saves with ≥1 MISSING owner | **0** |
| voice rows with nonzero owner | 5287 (ini 3740, gui 1547, script 0, music 0) |
| voice rows with owner 0 | 2590 (`carried_by=none`; 154 of those `playing=1`) |
| token == parsed == `AudioVoice1` tag count | 1665 |
| no `RuntimeGlobals` property (pre-audio saves) | 92 |
| unreadable / fixture zips (empty, missing Save.ini, not zip) | 24 |

Owner 0 is not counted as MISSING: the archive stores `owner=0` and there is no identity to carry.

### Representative saves with physical voices

| save | voices | missing | token / tags / leftover | notes |
|---|---:|---:|---|---|
| `D:\mx\s38\archive_loads\success\…\load_seed.ccsave` | 5 | 0 | 5 / 5 / 0 | parser validation target |
| `D:\mx\s40lanes\h4\peers_3_4_regression_20260909_105714\…\p5resync_56264.ccsave` | 12 | 0 | 12 / 12 / 0 | kept `peers_3_4` host resync |
| same family `p5resync_54452`, `_58156`, `_32464` | 12 | 0 | 12 / 12 / 0 | |
| same family `p5resync_56948` | 10 | 0 | 10 / 10 / 0 | |
| `D:\Projects\reviews\claude-review-2026-09-08\lanes\sound-ai-deferral\artifacts\source35-SoundContainerControl.ccsave` | 37 | 0 | 37 / 37 / 0 | AudioRuntime2 |
| `D:\mx\s41b4\j73\fresh\e2e\resync_heal\host\p5resync_22568.ccsave` | 1 | 0 | 1 / 1 / 0 | |
| W5 job `D:\mx\s41b3\j40\…` `p5resync_*` | — | — | — | not on disk (W5 §1.7 Index.ini stubs only) |

`phase1_summary.txt` has one line per of the 1847 paths. No line contains `MISSING`.

### Identity 9310 on retained saves

`saves_with_9310_in_ini=6`, `saves_with_9310_as_voice_owner=0` (`phase1_compact.txt`).

The six INI hits are the five `s40lanes\h4\peers_3_4_regression_20260909_11*\p5resync_*.ccsave` plus `source35-SoundContainerControl.ccsave`.

`clue_identity.py` on `p5resync_56264.ccsave` identity 9310:

```
preset=Metal Impact Machinery
copied_from=Metal Impact Machinery
files=Data/Base.rte/Sounds/Penetration/Metal/MetalImpactD*.flac
```

`Data\Base.rte\Effects\Wounds.ini:721-722` uses that preset as `BurstSound` on an `AEmitter` wound. The same save's playing voices include nearby ids (9317/9318 Brain Pod Hit, 9323 Metal Impact Machinery) and F2's 9124 (`Robot Death`), all `carried_by=ini`. Voice 36 on W5 named owner 9310; that blob is gone. On this kept family, 9310 is in the INI and is not a playing voice.

Playing voices on `p5resync_56264` (`validate_p5resync.json`): BattleRifle fire (9213), Robot Death (9124), Brain Pod Hit (9317/9318), Metal Impact Machinery (9323), plus owner 0 `GenericGibSmall3.flac`.

---

## Checker parsing evidence

Layout (read-only `D:\Projects\p4b-interp-validation\Source`):

- `.ccsave` is a zip (`ActivityMan.cpp:260-297`); `Save.ini` holds `RuntimeGlobals =` url-base64 (`215`) and `LuaStateGraph N|base64` (`246`).
- cpp-base64 `url=true` uses `.` for `=` padding (`SpecialBehaviour_PresetName = UDQgQWxwaGEgRHVlbA..`).
- Checkpoint text is a byte string (`CheckpointArchive.h`); decoded with latin-1 so lengths match.
- `AudioRuntime3` fields (`AudioMan.cpp:1395-1398`): voices are nested `AudioVoice1` (`AudioCheckpoint.h:224-234`) with `identity`, `owner`, `path`.
- INI carriers: `SpecialBehaviour_SoundCheckpoint` (`SoundContainer.cpp:274`).
- GUI: `GUISound1` / 27 `GUISoundContainer1` natives (`GUISound.cpp:161-199`).
- Music: `MusicMan1` nested `MusicSound1` natives (`MusicMan.cpp:358-411`).
- Script graphs: decoded `LuaStateGraph` scanned for `SoundContainerN` archives and `SpecialBehaviour_SoundCheckpoint`.

Validation on `load_seed.ccsave` (`validate_one.json`):

- decoded `RuntimeGlobals9` → `AudioRuntime3`
- `voice_count_token=5`, `voice_count_parsed=5`, `audio_voice1_tag_count=5`, `leftover_bytes=0`
- 126 INI sound checkpoints, 27 GUI sounds

Same three-way count match on `p5resync_56264` (12) and `source35-SoundContainerControl` (37). Sweep: 1665 saves have `voice_count_matches_token` and `voice_count_matches_audiovoice1_tags`.

Older `RuntimeGlobals1`–`4` have no `AudioRuntime` tag (`ActivityMan.cpp:1107-1128`; example `p5snap` decoded head `15 RuntimeGlobals4`, 14518 bytes, `AudioRuntime` absent). Those are recorded as 0 voices, not as parse failures.

---

## Phase 2

Not run. Gate in the brief: Phase 2 only if Phase 1 finds no save with voices, or finds MISSING owners but cannot name the class. Phase 1 found 1662 saves with voices and 0 MISSING owners.

Worktree `D:\Projects\audio-owner-registry` remains `stage2/audio-owner-registry` @ `9751a90e292eb300013c1d6052f3df7bbe228869`, clean. No commit.

---

## Classes that do not round-trip

None observed: no retained save has a nonzero voice owner absent from INI / script graph / GUI / music.

Source paths that can omit identity (not seen as a MISSING voice owner in this inventory):

| path | save | load |
|---|---|---|
| Lua-owned Entity as `copy`/`preset` name only | `LuaMan.cpp:3300-3308` (`lua_pushstring` `"copy"` / `"preset"` + class + preset + module; no `SpecialBehaviour_SoundCheckpoint`) | restore rebuilds from preset name; no persisted identity |
| Lua-owned native Entity INI | `LuaMan.cpp:3352-3365` `Writer::SnapshotScope` + `object->Save` / `Scene::SaveSceneObject` | `RestoreScriptGraphs` after Start (`ActivityMan.cpp:939-946`) |
| MO / Activity INI snapshot | `SoundContainer.cpp:274` `SpecialBehaviour_SoundCheckpoint` under `Writer::SnapshotScope`; MO objects borrowed live (`ActivityMan.cpp:219-225` `RetrieveSceneObjects(false)`) | `ReadProperty` `152-154` → `LoadCheckpoint` → `ReidentifyCheckpoint` |
| AEmitter wound burst (9310 on kept `p5resync`) | `AEmitter.cpp:289` `BurstSound`; identity via `SoundContainer.cpp:274` | `AEmitter.cpp:147-149` `ReadReflectedPreset` |
| GUI | `GUISound.cpp:196-199` `members[i]->SaveCheckpoint()` inside `RuntimeGlobals` | `GUISound.cpp:247-276` apply + manual registry insert |
| Music | `MusicMan.cpp:410-411` `source.SaveCheckpoint()` inside `RuntimeGlobals` | `MusicMan.cpp:634-653` `LoadCheckpointWithAudio` |

On every parsed voice with a nonzero owner, the carrier was `ini` or `gui`. Script-graph text on `p5resync_56264` contains `SoundContainer` userdata names (`SelfHeal`, `movementSound`, `defaultCorrectEntrySoundContainer`) and added identities 9064–9067; no playing voice named those ids.

---

## What this directory could not do

- Open the W5 returner blob (`p5resync_17092_recv`, 5409186 bytes). W5 §1.7: deleted by `RemoveSavedGame` (`NetMatchService.cpp:334`).
- Name the class of W5 voice-36 owner 9310 on that missing blob. On the kept `peers_3_4` family from the previous day, 9310 is `Metal Impact Machinery` and is in the INI.
- Run Phase 2 (instrumented save, heal e2e, `peers_3_4_regression` ×3, keep-switch). Gate not met. No worktree commit.
