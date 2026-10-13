#!/bin/bash
set -eu
export PATH=/usr/local/bin:/usr/bin:/bin:$PATH
unset CCCP_HEADLESS
# The joiner plays in a visible window on the box's own desktop.
export DISPLAY="${DISPLAY:-:0}"
exec /usr/bin/python3 "${2:?player tree}/tools/e2e_peers.py" --play "${1:?scene request}"
