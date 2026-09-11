from pathlib import Path
text = Path(r"D:\Projects\RESUME.md").read_text(encoding="utf-8")
start = text.find("# §D. HOW TO BUILD")
end = text.find("## ⚠️ If the GNS prefix")
print(text[start:end])
