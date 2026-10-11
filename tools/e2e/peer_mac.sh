#!/bin/zsh
set -eu
export PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH
unset CCCP_HEADLESS
export CCCP_POSIX_HOP=ssh
exec /usr/bin/python3 "${2:?player tree}/tools/e2e_peers.py" --play "${1:?scene request}"
