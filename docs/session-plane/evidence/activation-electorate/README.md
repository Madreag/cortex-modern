Activation electorate after an intervening hold
==============================================

FLAGGED fixture departure: ReturnThroughImage captured its survivor list when
the image began, then waited for every captured peer to install the return.
In both silent-voter cases the deliberate second fault holds seat 4 before the
activation. The engine correctly certifies return 2 with the still-playing
seats 1 and 3. Waiting on absent seat 4 prevented the certified joiner from
starting and caused a subsequent false rehold.

The helper now observes every required playing peer at the certified activation,
excluding held seats and certified neutral voters as the engine does. The final
live/no-rehold check applies to that activation's playing survivors. Every
original survivor's saved prefix is still checked unchanged. Seat 4's own
automatic recovery and the original row's continued-play/bound properties remain.

Both original REDS are retained under automatic-held-recovery on 089048ba.
Both are GREEN on 34841cd6, along with the authority oracle, healed tie,
second-drop split and different-donor return: 6/6 complete headed receipts.
No engine or single-player change. Topology single-box; proof=false.
The wider gate remains open; REPORT-final.txt is unchanged.
