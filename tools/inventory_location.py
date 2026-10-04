"""Where the lead's inventory tools are, for every check that reads them.

CC_INVENTORY_DIR names a copy (run_tools_suites.py --inventory sets it for each suite; a run sets it to the copy it
ships to a box); without it, the lead's own copy on EROL-PC. A shipped copy is flat, while the lead's own keeps its
operator scripts one level above the inventory.
"""

import os
from pathlib import Path

LEAD_INVENTORY = Path('D:/Projects/reviews/takeover-20260909/grok-workers/lead-tools/inventory')


def inventory_dir() -> Path:
    return Path(os.environ.get('CC_INVENTORY_DIR') or LEAD_INVENTORY)


def lead_script(name: str) -> Path:
    """An operator script of the lead tools: beside a shipped inventory, or one level above the lead's own."""
    inventory = inventory_dir()
    for path in (inventory / name, inventory.parent / name):
        if path.is_file():
            return path
    return inventory.parent / name
