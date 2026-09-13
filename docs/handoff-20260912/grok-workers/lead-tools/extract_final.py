"""Save a Grok CLI lane's final assistant message as REPORT-final-message.md beside its agent-output.jsonl."""
import json
import pathlib
import sys

lane = sys.argv[1]
run = pathlib.Path("D:/Projects/reviews/takeover-20260909/grok-workers/cli_runs") / lane
lines = (run / "agent-output.jsonl").read_text(encoding="utf-8", errors="replace").splitlines()
final = None
last_assistant = None
for line in lines:
    line = line.strip()
    if not line:
        continue
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        continue
    if obj.get("type") == "result" and isinstance(obj.get("result"), str):
        final = obj["result"]
    elif obj.get("type") == "assistant":
        msg = obj.get("message", {})
        content = msg.get("content", [])
        if isinstance(content, list):
            texts = [c.get("text", "") for c in content if isinstance(c, dict) and c.get("type") == "text"]
            if texts:
                last_assistant = "\n".join(texts)
        elif isinstance(content, str):
            last_assistant = content
text = final if final is not None else last_assistant
if text is None:
    print("no final message found")
    sys.exit(1)
out = run / "REPORT-final-message.md"
out.write_text(text.rstrip() + "\n", encoding="utf-8")
print("saved", out, len(text), "chars;", "result" if final is not None else "last assistant text")
