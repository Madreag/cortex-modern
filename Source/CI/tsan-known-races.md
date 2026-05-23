# TSan known-races filter list

Companion to the `determinism-tsan-linux` job in `.github/workflows/determinism.yml`.
Lists data-race reports that the TSan gate is allowed to ignore, with the
justification + the change that would remove each entry.

## Policy

- **Every entry needs an explicit justification + a removal path.** "Add to the
  filter list" is never the answer on its own — it has to come with what would
  change to take the entry back off.
- **No accumulation.** If the list grows past a handful of entries, that means
  the SyncedUpdate contract is being papered over and the audit (`Source/CI/path-e-audit-inventory.md`) needs to expand. Filter-list size is a
  signal, not a budget.
- **Reviewer expectation.** Any PR that adds an entry must also point at the
  audit row or open issue tracking its removal. Empty justification = bounce.

## Entry format

Each entry is three labelled blocks:

````markdown
### <short-name>

**Race signature:**
```
WARNING: ThreadSanitizer: data race ...
  <one or two frames from the TSan stack — enough to identify the call site>
```

**Justification:**
One paragraph. Why this race is benign or out-of-scope for the gate today. Cite
the audit row, ADR, or issue that explains the call pattern.

**Removal path:**
What concrete change closes this entry — relocate caller X to SyncedUpdate, land
fix Y, finish audit row Z. One short sentence.
````

## Entries

_None yet._ The list starts empty. The TSan gate is advisory at this point, so
new races surface in CI output rather than getting silently filtered.

## Promoting the gate to required

The advisory phase reports every race; nothing in this file is actually consumed
by TSan. When the gate flips to required (drop `continue-on-error` from the
`determinism-tsan-linux` job), entries here that must silence TSan need a
machine-readable companion at `Source/CI/tsan-suppressions.txt` written in TSan's
own syntax (`race:<function-or-symbol-pattern>` per line), and the workflow's
`TSAN_OPTIONS` extended with `:suppressions=$GITHUB_WORKSPACE/Source/CI/tsan-suppressions.txt`.
This file remains the human record; the .txt is what TSan reads. Keep them in
lockstep — adding to one without the other will surprise the next reviewer.
