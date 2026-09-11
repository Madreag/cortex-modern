from pathlib import Path

src = Path(r"D:\Projects\RESUME.md")
out = Path(r"D:\Projects\reviews\takeover-20260909\grok-workers\w12-windows-compile\section-d.txt")
text = src.read_text(encoding="utf-8")
start = text.find("# §D. HOW TO BUILD")
end = text.find("## The verification contract")
if start < 0 or end < 0 or end <= start:
    raise SystemExit(f"markers missing start={start} end={end}")
chunk = text[start:end]
out.write_text(chunk, encoding="utf-8")
print(f"wrote {out} bytes={len(chunk.encode('utf-8'))} start_char={start} end_char={end}")
print("--- BEGIN SECTION D ---")
print(chunk, end="")
print("--- END SECTION D ---")
