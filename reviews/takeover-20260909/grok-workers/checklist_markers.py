import io
import re

L = io.open(r"D:\Projects\RESUME.md", encoding="utf-8").read().split("\n")
s = next(i for i, l in enumerate(L) if l.startswith("## §B-1 THE COMPLETION CHECKLIST"))
e = next(i for i, l in enumerate(L) if l.startswith("## §B-0 CORRECTIVE MILESTONE"))
sec = None
out = {}
DONE = re.compile(r"✅|\bdone\b|\bshipped\b|\bclosed\b|\bpass(?:es|ed)?\b|\bgreen\b")
OPEN = re.compile(r"\bopen\b|\bred\b|\btodo\b|\bpending\b|\bnot (?:yet|done|started|verified)\b|\bremain(?:s|ing)?\b|\bunverified\b")
for l in L[s:e]:
    m = re.match(r"### (\d)\. (.*)", l)
    if m:
        sec = m.group(1) + " " + m.group(2)[:58]
        out[sec] = {"lines": 0, "done": 0, "open": 0}
        continue
    if sec and l.strip():
        out[sec]["lines"] += 1
        t = l.lower()
        out[sec]["done"] += len(DONE.findall(t))
        out[sec]["open"] += len(OPEN.findall(t))
for k, v in out.items():
    print(f"{k:62} lines={v['lines']:3} done={v['done']:3} open={v['open']:3}")
