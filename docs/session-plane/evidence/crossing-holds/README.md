# A two-player match never freezes when both seats are held at once (#51)

Linux (GCC, v6 network library), headed engines through the POSIX runner, topology single-box, proof=false.

## The reported freeze (`reported-freeze-lines.txt`)

Two engines on one four-core box at a fixed one-tick input delay, both slowed by a save's graph walk. The host volunteered
its own hold (frame 8091, co-signed by the client) and 13 ms later - before installing it, its own walk over and the client's
still running - proposed holding the client as a slow machine (8093), which the client co-signed too. Both peers installed
both holds: `members=0`, each peer a private reader of the other, `net-peer-recovery ... applied=8092 donor_applied=8092`
for good.

## A second way the same match freezes (`scene-runs.txt`)

Driver: `tools/test_crossing_holds.py` - host and client, Fixed 1, each simulation slowed over its own overlapping tick window
(`CCCP_TEST_SIM_COST_US` with `FROM`/`UNTIL`). Pass = both peers play past the end tick; a seat was held and returned; no
round without a playing seat; live hash compares agree, after the losing half's image rejoin when the machines split; normal
exits.

With `--host-load 25000:1500:1560 --client-load 40000:1540:1660` each machine holds the other locally at one frame (a split
in a two-seat round). The healed histories rule for the host and the client stops to return through the image, but on the
host the in-place catch-up opener refused the client's reports (`transport-bound`, twelve times): it predates frame-group
rounds and read any bound link as a seat still playing, while a frame-group round keeps a held seat's own link bound. The
client stood still at 2100 while the host played to the end: RED 2/2 on 56b4fb9b (`the round stood still for 90 s at
ticks (12000, 2100)`).

Once the opener accepted the held seat's own link, the loser replayed the host's frames over its own diverged world (the live
compare had already flagged frames 2100 and 2130): its stop named the frame it stopped at, not where its history parted.

## The fix

- A seat never signs a hold - proposed, co-signed in reply or from a stored vote - that, with the holds it has signed or
  installed, would hold every seat of the round, or that would have a seat held or holding itself hold another. A hold that
  would leave no playing seat is not installed either: the seat it names plays on alone, the other is held and returns. A
  reader still takes the history it reads and the ruling half's hold (`catalogue-regression.txt`: the first form of the
  install refusal also refused those, and a four-seat catalogue case went RED on it; GREEN again on 1b068d9a).
- (c) A held seat that no seat can give its next frame - every other seat silent past the host-loss window, or held itself
  - takes its seat back and holds the others at that frame; of several such seats only the original host, else the lowest,
  does, and the others read its history. A lone survivor then migrates as before.
  A seat whose applied frame still rises is reading toward its own return and is waited for: the first form took the round
  from such a seat (random seed 92, `catalogue-regression.txt`: RED on 8466d30b, GREEN on 8c903518).
- The host's in-place opener accepts a held seat's own bound link in a frame-group round.
- A split loser records the frame after the split's last common frame as where its history parted, so it rejoins through
  the image, as the ruling says, never by replaying the ruling half's frames over its own.
- (a) A returner's pace is compared with this machine's display one round trip earlier (what it applied, this machine had
  shown that long before) and barred at the round's nominal rate: a catch-up burst above 60 is not the bar it must meet.

Native row `a_round_keeps_a_playing_seat` (net-lockstep): RED on dd6923e8 with the guard disabled by a temporary lever
(`the hold that left the round without a playing seat was installed: held=1 members=0`), GREEN on 548610a7; its positive
controls (a lone own hold, an ordinary hold, a co-signed hold with the host playing, an install that leaves a third seat)
pass in both (`native-row-red-green.txt`). Scene GREEN on the same engine (`xh-h1`: split, image rejoin complete at 3946,
no compare difference after it; `xh-h2`: no split), and twice more on 1b068d9a (`g1`, `g2`: split, image rejoin complete at
3238 and 3230, no compare difference after it).

Native row `a_held_seat_with_no_donor_plays_on`: RED on 48f33826 with the lone path disabled by a temporary lever (`a held
seat with no seat left to give its history still waited`), GREEN without it; its controls (a held seat a playing seat still
feeds waits; of two held seats only the host plays on) pass. Scene, `--host-dies-when-client-held`: the client slowed until the
host holds it, then the host engine dropped by the runner. RED on 1b068d9a (`the round stood still for 90 s at ticks (1500,)`),
GREEN on 8466d30b: the client read the host's history to 1514, waited out the host-loss window, took its seat back at 1515
(`lone=1`), migrated to host at 1520 and played to 2700.

Driver corrections between runs, disclosed: the first version started the client without a reconnect ticket (every real
client has one; its image rejoin failed `No reconnect record for that match`), counted the split's own pre-ruling compare
differences as a failure, and stopped the match at the end tick while the loser was still replaying; it now passes the
client a ticket, judges the compares after the rejoin, and waits for the rejoin and 300 live ticks after it. The host-drop mode
was added before its first run; its RED and GREEN ran the same driver.

Open, measured: the loser's image rejoin took about 30 s of round time in `xh-h1` (split at 2162, complete at 3946) and about
19 s in `g1`/`g2` (2064 to 3238, 2096 to 3230) on a box running both engines; the survivor played on throughout. A held seat
whose host is gone waits out the 15 s host-loss window before it plays on alone, the same window a playing survivor waits
before it migrates.
