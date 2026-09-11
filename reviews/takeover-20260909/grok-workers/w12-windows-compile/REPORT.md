# W12 Windows compile (`stage2/takeover-msvc`)

## Build

| Item | Value |
|---|---|
| Result | LINK OK (`exit_code=0`, 76.942 s) at HEAD `be0a2672a9` |
| Tree | `D:\Projects\takeover-build` branch `stage2/takeover-msvc` |
| Recipe | RESUME §D; `GNS_ROOT` / `GNS_DEP_ROOT`; `CL=/MP6`; `/t:RTEA` Final x64 |
| Log | `build-iter2.log` / `build.log` |
| Exe | `D:\Projects\takeover-build\Cortex Command.exe` |
| Exe size | 19223552 |
| Exe sha256 | `992b14992a6e8e7e5b6e49899e9171a8dfc1678013dcdc3769b408a036b2652d` (`hash_exe.py`, `exe-meta.json`) |
| Worker commits | none |
| Vendor `_Bin` | `allegro-release.lib`, `luabind-release.lib` unstaged |

Warnings on the linking rebuild (`build-iter2.log`): 10 `MSB4011`, 11 `warning C`.

## Iterations

1. Rebuild at `6fa0fd6898` — C2888 in `LuaMan.cpp`. Stopped (then a class; lead later allowed it). No worker edit.
2. Rebuild at lead `be0a2672a9` — linked. No worker edit.

`git diff HEAD~1 HEAD --stat` (lead C2888 placement): `Source/Managers/LuaMan.cpp | 4 ++++`

## Selftests

Launches: `python ...\run_selftests.py` → `tools\win32_test_runner.py` with Mac argv. Evidence under `D:\mx\w12\<name>\` (`stdout.log`, `launch.json`). Scratch excluding Data junctions: 56 files, 0.22 MiB (`size_mx_w12.py`).

| name | result | path |
|---|---|---|
| net-protocol | `[net-protocol-selftest] PASS` exit 0 | `D:\mx\w12\net-protocol` |
| net-identity | `[net-identity-selftest] PASS` exit 0 | `D:\mx\w12\net-identity` |
| net-session | `[net-session-selftest] PASS` (plus codec line) exit 0 | `D:\mx\w12\net-session` |
| net-lockstep | `[net-lockstep-selftest] PASS` (79 verdict lines, 0 FAIL) exit 0 | `D:\mx\w12\net-lockstep` |
| net-match | `[net-match-selftest] PASS` exit 0 | `D:\mx\w12\net-match` |
| net-auth | `[net-auth-selftest] PASS` exit 0 | `D:\mx\w12\net-auth` |
| net-admission | `[net-admission-selftest] PASS` exit 0 | `D:\mx\w12\net-admission` |
| net-reconnect | `[net-reconnect-selftest] PASS` exit 0 | `D:\mx\w12\net-reconnect` |
| net-reconnect-session | `[net-reconnect-session-selftest] PASS` (plus 5 GATE PASS) exit 0 | `D:\mx\w12\net-reconnect-session` |
| net-discovery | `[net-discovery-selftest] PASS` exit 0 | `D:\mx\w12\net-discovery` |
| controller-frame | `[controller-frame-selftest] PASS` exit 0 | `D:\mx\w12\controller-frame` |
| native-graph | engine exit 1; 3 FAIL / 640 verdict lines | `D:\mx\w12\native-graph` |

Native-graph FAIL lines verbatim (`D:\mx\w12\native-graph\stdout.log`):

```
[net-local-ui-selftest] FAIL unreferenced_owner_reclaimed
[script-graph-selftest] FAIL native_runtime_checkpoint_values
[script-graph-selftest] FAIL
```

Mac positive20 log has PASS on the first two names. All PASS/FAIL lines: `verdicts-*.txt`. Summary: `selftest-summary.json`.

Runtime deps staged beside the exe (not committed): `fmod.dll` from this tree, GNS `*.dll` from `GNS_DEP_ROOT\bin`. `Userdata\Settings.ini` copied from Mac `Settings.input.ini` for `prepare_runtime`.

No push. No source commit.
