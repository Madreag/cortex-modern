"""Write host/client setup-editor scripts via net_activity_launch.editor_script."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from net_activity_launch import WAITING_SEEN_SIGNAL, editor_script  # noqa: E402


def main(argv=None) -> int:
    out = Path(argv[0] if argv else sys.argv[1]).resolve()
    host_dir = out / "host-ui"
    client_dir = out / "client-ui"
    host_dir.mkdir(parents=True, exist_ok=True)
    client_dir.mkdir(parents=True, exist_ok=True)
    host_signal = host_dir / (WAITING_SEEN_SIGNAL + ".json")
    for peer, dest in (("host", host_dir), ("client", client_dir)):
        delay = 0
        payload = editor_script(
            peer,
            False,
            delay,
            False,
            False,
            None,
            host_signal=host_signal,
        )
        # Headless service-e2e AfterDraw leaves render=0, so editor_script's
        # wait-renders never completes and the 120-tick editor cap fires.
        for step in payload.get("steps", []):
            if step.get("op") == "wait" and "renders" in step:
                step["sim_at_least"] = int(step.pop("renders"))
        (dest / "ui-script.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(host_dir / "ui-script.json")
    print(client_dir / "ui-script.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
