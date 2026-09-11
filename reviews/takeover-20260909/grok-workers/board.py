"""Update one row of D:\\Projects\\STATUS.md and append a history line.

usage: python board.py <item> <pct> <delta> <working_on> <next> <overall> <history_note> [utc_time]
"""
import datetime
import io
import sys

S = r"D:\Projects\STATUS.md"
item, pct, delta, work, nxt, overall, note = sys.argv[1:8]
when = sys.argv[8] if len(sys.argv) > 8 else datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
s = io.open(S, encoding="utf-8").read()
key = f"| {item} | "
a = s.index(key)
b = s.index("\n", a)
name = s[a:b].split("|")[2].strip()
s = s[:a] + f"| {item} | {name} | {pct} | {delta} | {work} | {nxt} |" + s[b:]
la = s.index("Last update:")
lb = s.index("\n", la)
s = s[:la] + f"Last update: {when} · Overall **{overall}%** ({note})" + s[lb:]
s = s.rstrip("\n") + f"\n- {when} · overall {overall} · {item}:{pct} · {note}\n"
io.open(S, "w", encoding="utf-8", newline="\n").write(s)
print("board:", item, pct, delta, overall)
