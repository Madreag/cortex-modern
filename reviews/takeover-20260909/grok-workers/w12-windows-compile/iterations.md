# W12 MSVC fix iterations

Tree: `D:\Projects\takeover-build` branch `stage2/takeover-msvc`.
Vendor `_Bin\*.lib` stay unstaged.

Lead starting point: `6fa0fd6898` renamed file-local `Writer()` to `DrainJournal` (`NetA7Journal.cpp:184/263`).

## Iteration 1

Command: `python D:\Projects\reviews\takeover-20260909\grok-workers\w12-windows-compile\run_build.py`
Log: `build-iter1.log` (also current `build.log`)
Meta: `exit_code=1` `duration_seconds=13.047`
HEAD at rebuild: `6fa0fd6898`

Errors (verbatim from `build.log`):

```
D:\Projects\takeover-build\Source\Managers\LuaMan.cpp(3570,23): error C2888: 'bool RTE::LuaStateWrapper::HasNativeAliases(const std::unordered_set<const void *,std::hash<const void *>,std::equal_to<const void *>,std::allocator<const void *>> &)': symbol cannot be defined within namespace 'anonymous-namespace'
D:\Projects\takeover-build\Source\Managers\LuaMan.cpp(3598,23): error C2888: 'bool RTE::LuaStateWrapper::RekeyScriptObjects(const std::vector<std::pair<const RTE::MovableObject *,long>,std::allocator<std::pair<const RTE::MovableObject *,long>>> &,bool)': symbol cannot be defined within namespace 'anonymous-namespace'
```

Quoted source:

```
268:namespace {
...
3570:bool LuaStateWrapper::HasNativeAliases(const std::unordered_set<const void*>& objects) {
...
3598:bool LuaStateWrapper::RekeyScriptObjects(const std::vector<std::pair<const MovableObject*, long>>& identities, bool validateOnly) {
...
3726:} // namespace
3728:void LuaStateWrapper::LoadScriptGraphHelper() {
```

Analysis: file-local helpers from line 268 to 3726 live in an anonymous namespace. Two `LuaStateWrapper` member definitions sit inside that block. GCC 13.4 accepted them (members still attach to the class). MSVC C2888 refuses a class member definition inside an anonymous namespace. The other `LuaStateWrapper::` / `LuaMan::` definitions start at 3728, after the close.

This is not an allowed class: not a missing include, not a file-local helper rename, not `typename`/`template`/`this->`, not designated-initializer order, not a `std::` substitute, not a brace-init narrowing cast. Closing the anonymous namespace around those two members would be a placement change of public member definitions. Loop stopped. No edit, no commit.

`git diff HEAD~1 HEAD --stat` not run for a new worker commit (none made). Lead commit `6fa0fd6898` remains HEAD.

## Iteration 2

Command: `python D:\Projects\reviews\takeover-20260909\grok-workers\w12-windows-compile\run_build.py`
Log: `build-iter2.log` / `build.log`
Meta: `exit_code=0` `duration_seconds=76.942`
HEAD at rebuild: `be0a2672a9`

Errors: none. Link:
```
RTEA.vcxproj -> D:\Projects\takeover-build\Cortex Command.exe
```

Exe: `19223552` bytes, sha256 `992b14992a6e8e7e5b6e49899e9171a8dfc1678013dcdc3769b408a036b2652d` (`hash_exe.py`).

No worker fix. No worker commit. `git diff HEAD~1 HEAD --stat` is the lead C2888 placement commit `be0a2672a9` (`Source/Managers/LuaMan.cpp | 4 ++++`).

Selftests after this link: ten network CLI + controller-frame all runner/engine exit 0. native-graph engine exit 1 with three FAIL lines in `native-graph-fails.txt`. Full verdict lists in `verdicts-*.txt`.


