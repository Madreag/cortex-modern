---
name: bash-heredoc-hook-mangles-backslashes
description: "The Bash tool's PreToolUse hook rewrites backslash sequences and Windows paths inside command text (heredocs included), so scripts with backslashes must be written with the Write tool"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 33bbc5d3-b3a3-4ba2-9e2b-aa59ec12ad01
  modified: 2026-09-06T07:26:12.563Z
---

On this machine the Bash tool's PreToolUse hook edits the command text before it runs: `\\n`, `\\r`, `\\C`, `\\P` and similar sequences inside heredocs get collapsed, JSON escapes in arguments get broken, and backslash paths are converted to forward slashes. A Python or PowerShell script passed through a heredoc that contains any backslash therefore fails to match its anchors or gets a syntax error.

**Why:** three separate sessions lost time to it (the resplit driver, the harness patch, a JSON argument with `\\\\e2e`).

**How to apply:** write any script that contains backslashes (Windows paths, `"\n"` string literals, regexes) with the Write tool and run it by path; pass values to it as forward-slash paths or via a file, never as escaped JSON on the command line. Related: [[commit-attribution-user-only]].
