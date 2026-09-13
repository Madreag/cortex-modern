"""Apply one hunk of `git diff <base> <other> -- <file>` (chosen by its @@ header prefix) to the working file by content."""
import pathlib
import subprocess
import sys

tree, base, other, path, header = sys.argv[1:6]
diff = subprocess.run(["git", "-C", tree, "diff", base, other, "--", path], capture_output=True, text=True, encoding="utf-8", errors="surrogateescape").stdout
lines = diff.split("\n")
start = next(i for i, l in enumerate(lines) if l.startswith(header))
end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("@@")), len(lines))
hunk = lines[start + 1:end]
old = [l[1:] for l in hunk if l.startswith((" ", "-"))]
new = [l[1:] for l in hunk if l.startswith((" ", "+"))]
while old and old[-1] == "" and new and new[-1] == "":
    old.pop(); new.pop()
target = pathlib.Path(tree, path)
text = target.read_text(encoding="utf-8", errors="surrogateescape")
crlf = "\r\n" in text
body = text.replace("\r\n", "\n").split("\n")
matches = [i for i in range(len(body) - len(old) + 1) if body[i:i + len(old)] == old]
if len(matches) != 1:
    print("hunk context found", len(matches), "times; not applied")
    sys.exit(1)
i = matches[0]
body[i:i + len(old)] = new
out = "\n".join(body)
target.write_text(out.replace("\n", "\r\n") if crlf else out, encoding="utf-8", newline="")
print("applied hunk", header, "at line", i + 1, ":", len(old), "->", len(new), "lines")
