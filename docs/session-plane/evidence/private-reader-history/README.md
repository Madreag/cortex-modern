# A seat catching up privately plays only the history it reads

Linux (GCC, v6 network library), native and fixture self-tests through the POSIX runner, headed, single-box, proof=false.

Defect (catalogue group `hold_not_immediate_ai`, 4 fixed cases on 22876760, more in the random schedules): seed 169 - the
host goes silent, seat 2 holds it at frame 326 alone, the host holds seat 2 at 334, and seat 3 (held as a slow machine since
255) is catching up privately. Seat 3 first reads seat 2's tail, then switches to the host's tail at 8058 ms. Seat 2's direct
certificate holding the host at 326 had been kept for its frame; at 326 seat 3 installed it and then applied the host half's
certified record, in which the host plays: `a held incarnation supplied human input at frame 326 ... observer=3 seat=1
hold_frame=326` (`catalogue-lines.txt`).

Three simpler rules were tried and withdrawn: refusing every direct hold (8e4c3d860, reverted by 65ba74fba) left a reader
stranded on the half the directory did not select (`four_seats_hold_during_another_seats_private_catch_up`: the directory
ruled seat 3's lone half, the reader kept the other half's records and stalled at 719 until a late image return); adopting a
direct hold once ruled still let the reader apply the other half's records afterwards; adopting only ruled holds refused an
unsplit hold of a stopped seat (random seed 17), after which the reader judged the holding group's records as lacking
authority and stalled.

The rule (this change): a private reader adopts a hold certified to it directly before it has shown that frame, unless the
split's ruling names another group or, with no ruling, the record it already has of that frame (its committed tail ahead, or
its own prepared frame) shows a held seat playing - then the hold is another continuation's. It asks the referee about the
split either way, and the existing path discards its unplayed preparation from an adopted hold's frame. Holds of the history it
reads also come with the tail's own record. It never applies a record in which a seat it holds at that frame plays: such a
record is set aside and the frame asked of another donor.

Native row `a_private_reader_plays_only_the_history_it_reads` (net-lockstep, six arms): RED on a51031e3 with both checks
disabled (arms 0, 1, 3 adopt another continuation's hold; the foreign record is applied and re-asked of the same donor),
GREEN on 56b4fb9b; arms 2, 4 and 5 (the ruled hold before its frame is shown; the hold carried by the record; an uncontested
hold) install in both (`native-row-red-green.txt`).
