export PATH="/opt/homebrew/bin:/usr/local/bin:/Users/erol/.local/bin:$PATH"
export HOME=/Users/erol/cortex-workers/cli-home
cd /Users/erol/cortex-workers/takeover-next-clean || { echo 90 > exit.txt; exit 90; }
/Users/erol/.local/bin/cursor-agent -p --force --trust --sandbox disabled --output-format stream-json \
  --model cursor-grok-4.6-xhigh-fast \
  --workspace /Users/erol/cortex-workers/takeover-next-clean \
  "$(tr -d '\r' < prompt.txt)" > agent-output-w22.jsonl 2> agent-stderr-w22.txt
code=$?
echo "$code" > exit.txt
exit "$code"