"""Locate optional inventory tools beside a shipped copy or in a configured directory.

CC_INVENTORY_DIR selects a shipped flat copy. CCCP_INVENTORY_DIR selects an operator
inventory whose scripts may also live one level above it. Without either, use this folder.
"""

import os
from pathlib import Path

LEAD_INVENTORY = Path(os.environ["CCCP_INVENTORY_DIR"]) if os.environ.get("CCCP_INVENTORY_DIR") else None


def inventory_dir() -> Path:
    return Path(os.environ.get('CC_INVENTORY_DIR') or LEAD_INVENTORY or Path(__file__).resolve().parent)


def lead_script(name: str) -> Path:
    """An operator script beside a shipped inventory or above a configured operator inventory."""
    inventory = inventory_dir()
    locations = (inventory, inventory.parent) if LEAD_INVENTORY is not None and inventory == LEAD_INVENTORY else (inventory,)
    for location in locations:
        path = location / name
        if path.is_file():
            return path
    return locations[-1] / name
