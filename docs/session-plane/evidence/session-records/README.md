# Session records in a match are bounded

The round's per-frame stores fill for the first 4,096 frames (c_PeerHistoryFrames, about 68 s) and then stay there; the world
history keeps 3,600 frames and evicts the rest. Measured in a two-peer match with no fault, 10,800 ticks, the memory census every
60 ticks (`census.json`; the census now also counts the peer-frame stores it did not name before).

At ticks 1,800 / 3,600 / 4,500 / 10,800 on the host: MigrationHistory 1,781 / 3,581 / 4,096 / 4,096; PeerFrameWitnesses (two per
frame) 3,560 / 7,161 / 8,191 / 8,192; PeerPreparedPrefixes, PeerRejectedInputs and PeerInputSentAtMs follow MigrationHistory;
the world history holds 3,600 frames from tick 3,600 with 7,199 evicted by the end. Every other round, runner, world and lobby
store stays under 1,000 records the whole match. The kept history costs 2,603,739 bytes at its cap; the heap in use grows 10 MB
while it fills and 5 MB (host) / 3 MB (client) over the following 105 s.

So the growth seen in a short window is the fill to the cap, about six records a frame in a two-peer round (three a frame are
the history and its two witnesses), and it stops at 68 s.
