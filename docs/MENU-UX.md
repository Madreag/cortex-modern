Directory waiting during joining

When the directory asks the host to wait, Joining shows: “The directory is busy. The host retries in N seconds. Cancel returns to Multiplayer.” The count follows the actual remaining wait. A pending seat stays reserved for that wait and admission resumes afterward.

When the directory confirms that a match has ended or is gone, Joining offers another game. A host’s own End Match shows the completed round’s result, including “Match ended by host.”

A player whose own seat is held

The in-match overlay and the hold notification both say: “The AI is playing for you until you are back”. This applies to the host and to a joining player. The roster decides whether the local seat is held, including while its connection remains live. The held player’s probe checks this same visible sentence. A returning seat remains held while it catches up; the notice changes when the agreed return gives the player control again.

Live input delay

The diagnostic in-match overlay says: “Input delay N ticks / M ms”, with milliseconds to one decimal place. N is the local input delay in force on the frame being drawn. The line updates when an agreed re-size takes effect, including on the smaller-screen layout. The screen probe compares this visible line with the engine's current local delay.

The automatic-delay hint says: “measured ping and recent jitter, re-sized live”. The slow-player bound is a separate setting: holding a slow player's seat does not add that machine's compute time to everyone's input delay.
