export PATH="/opt/homebrew/bin:/usr/local/bin:/Users/erol/.local/bin:$PATH"
export HOME=/Users/erol/cortex-workers/cli-home
cd /Users/erol/cortex-workers/cli-smoke || { echo 90 > exit.txt; exit 90; }
rm -f a.txt b.txt
( /Users/erol/.local/bin/cursor-agent -p --force --trust --sandbox disabled --output-format text --model cursor-grok-4.6-xhigh-fast --workspace /Users/erol/cortex-workers/cli-smoke --add-dir /Users/erol/Documents/Codex/cortex-b2-review-20260909 "List the names of the first three entries of /Users/erol/Documents/Codex/cortex-b2-review-20260909 and write them to a.txt in the current directory, then reply done." > t_adddir.txt 2>&1; echo "adddir_exit=$?" >> t_adddir.txt ) &
p1=$!
( /Users/erol/.local/bin/cursor-agent -p --force --trust --sandbox disabled --output-format stream-json --model cursor-grok-4.6-xhigh-fast --workspace /Users/erol/cortex-workers/cli-smoke "Write the single line STREAM_OK to b.txt in the current directory and reply done." > t_stream.jsonl 2>t_stream.err; echo "stream_exit=$?" >> t_stream.err ) &
p2=$!
wait $p1 $p2
echo "a.txt: $(cat a.txt 2>&1 | head -3 | tr '\n' '|')" > t_summary.txt
echo "b.txt: $(cat b.txt 2>&1)" >> t_summary.txt
echo "stream_events=$(wc -l < t_stream.jsonl)" >> t_summary.txt
echo 0 > exit.txt