export PATH="/opt/homebrew/bin:/usr/local/bin:$HOME/.local/bin:$PATH"
export HOME=/Users/erol/cortex-workers/cli-home
cd /Users/erol/cortex-workers/cli-smoke || { echo 90 > exit.txt; exit 90; }
/Users/erol/.local/bin/cursor-agent status > smoke2.txt 2>&1
echo "status_exit=$?" >> smoke2.txt
/Users/erol/.local/bin/cursor-agent -p --force --trust --sandbox disabled --output-format text --model cursor-grok-4.6-xhigh-fast --workspace /Users/erol/cortex-workers/cli-smoke "Create a file named hello.txt in the current directory containing exactly the line SMOKE_OK and reply with the single word done." > smoke2-agent.txt 2>> smoke2.txt
echo "agent_exit=$?" >> smoke2.txt
cat hello.txt >> smoke2.txt 2>&1
echo 0 > exit.txt