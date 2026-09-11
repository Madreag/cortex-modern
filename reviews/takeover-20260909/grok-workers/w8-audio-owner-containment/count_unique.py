import json
from collections import Counter
from pathlib import Path
inv = json.loads(Path("save_inventory.json").read_text())
c = Counter(r["size"] for r in inv["saves"])
print("saves", len(inv["saves"]), "unique_sizes", len(c), "singleton_sizes", sum(1 for n in c.values() if n == 1))
print("largest groups", c.most_common(8))
