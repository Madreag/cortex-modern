# Contributing

Cortex Modern is a fork of the [Cortex Command Community Project](https://github.com/cortex-command-community/Cortex-Command-Community-Project) that adds online multiplayer. Fixes and features for multiplayer belong here. Changes that the original project would want (engine fixes, determinism work, tooling) are prepared here first and then offered upstream as small, focused pull requests.

## The rules this fork works by

These come from the Community Project's maintainers and are not negotiable here either.

- **Controller-sync multiplayer.** Every machine runs its own AI and pathfinding. Only player input crosses the network. A change that makes the AI itself deterministic across machines is out of scope.
- **Determinism through standard floating point.** Cross-platform agreement comes from fixed compiler floating-point settings and deterministic math at the primitive level, never from a fixed-point rewrite.
- **Mods keep working unchanged.** No change may require a mod author to edit a working script. Lua behaviour that scripts can observe (live references, closures, shared tables, coroutines) stays as it is.
- **Single player keeps its feel.** Timing, pacing and input changes for multiplayer are switched on only inside an online match.
- **One purpose per change.** A commit does one thing that fits in one sentence. A fix, a nearby refactor and an unrelated cleanup are three commits.

## Before you open a pull request

1. Build the tree (see [Building](README.md#building)). Multiplayer needs the networking library; a build without it cannot host or join.
2. Run the engine's self-tests and the tool suites from the repository root:

   ```
   python tools/run_selftests.py --repo . --out <a scratch folder> --timeout 300
   python tools/run_tools_suites.py --repo .
   ```

   Both print one line per test and a total. On a clean tree every line reads PASS; a few tool suites read NOT APPLICABLE away from the project's own test machines.
3. A fix comes with a test that fails without it. Say in the pull request which test that is.
4. If you touched anything two players must agree on (a network message, a version number, saved state), say so at the top of the pull request.

## Code style

- Follow the surrounding code and the repository's `.clang-format`.
- Comments are short and say why, not what. No history in comments ("this used to..."), no ticket numbers.
- Public declarations in headers carry a `///` description.

## Commit messages

Plain words: what changed and why. The author field is the record of who wrote it, so keep trailers out of the message.

## Reporting bugs

See [Reporting a bug](README.md#reporting-a-bug). For anything that could be abused against other players, use the private route in [SECURITY.md](SECURITY.md) instead of a public issue.

## License

Contributions are accepted under the repository's license, the GNU Affero General Public License v3 (see `LICENSE`).
