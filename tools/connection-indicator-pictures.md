The design of record is section 3.8 of [MENU-UX.md](../MENU-UX.md).

`test_connection_indicator.py` runs the existing hidden runner and e2e scene driver. Every HUD state has its own case at 640x360, 960x540, 1280x720 and 1920x1080. Every Seats case has four seats in four different states and checks every column on the host and clients. The quality lever deliberately makes 250 ms Good and 80 ms Substituting. Exact strings and RGB expectations are written independently of the presentation functions.

The native picture assertions measure text, containment, overlapping controls and ink read from the rendered frame. Each case retains both the HUD bitmap and composited frame. F6 presses and releases have two drawn frames between them. Drop-down selection uses the existing framed HandPick gesture. The toggle case saves Off, relaunches into a match with no badge and the full Seats column, then saves On and relaunches again before checking the badge.

The single-player guard pauses a seeded fixture at its fourth tick, captures the whole composited frame and compares all RGB pixels to the baseline. There is no tolerance or mask. The baseline guard is the unchanged reference; it cannot honestly be a failing test of its own pixels.

Run the driver with `--repo <TIP_TREE> --baseline-repo <BASE_TREE> --out <NEW_OUTPUT> --peer-boxes host=<BOX>,seat2=<BOX>,seat3=<BOX>,seat4=<BOX> --host-address <HOST_ADDRESS> --pool-registry <BOX_REGISTRY>`. The baseline checkout must be 107dbdab7c7d780d40f6be4fb8f27e49db9e7a3a and hold its verified executable. A single-engine assignment uses `--single-engine` and reports the multiplayer pictures as NEEDS TESTING. It never starts extra peers on that box or reports them green.
