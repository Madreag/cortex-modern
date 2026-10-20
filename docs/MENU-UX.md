# Multiplayer menus

How the multiplayer screens behave, for players and for anyone changing them. Every screen follows the same rules:

1. **The first screen of a task holds only what a newcomer must decide.** Everything else starts on the best choice for an
   ordinary home connection and PC and sits behind one `Advanced` step.
2. **Game setup is ordinary; network tuning is advanced.** What to play, where and with whom is changed through `Edit setup`.
   Ports, delays, relays, recovery and session controls are behind `Advanced`.
3. **Words a player uses, and only promises the game keeps.** Internal names, script lines and raw session errors never reach
   a player; the log and the support report keep them.
4. **One line per state.** A state is said once on screen, with what happens, why, and what to press.
5. **Three scopes are kept apart.** This match, the games this computer hosts, and this computer: a page says which one it edits.
6. **Every control works under a hand at every supported size.** Tests press and release on separate frames with the screen
   refreshing between, and check the picture at 640x360, 960x540 and 1280x720.

## Host a Game and the lobby

- Host a Game asks for the activity, scene, mode and number of players, then `Create Lobby`. The lobby is listed in the
  online game list by default.
- In the lobby the host has `Edit setup` (the Players and Rules pages, titled EDIT SETUP) and `Advanced` (every page, titled
  ADVANCED). Everyone else has `Match details`, a read-only view of every page.
- **Ready, then Start.** Every player but the host presses `Ready` and may take it back with `Cancel Ready`. When everyone is
  ready the host's `Start Match` starts at once. When someone is not, `Start Match` begins a 30-second countdown that every
  player sees ("The host is starting the match in 27 s - press Ready"); the host's button becomes `Cancel Start`. The last
  Ready starts the match at once; a Ready taken back does not stop the countdown; at zero the match starts with everyone in
  the lobby. Opening the setup stops a running countdown, and a setup change clears Ready ("The host changed the setup -
  check it and press Ready again").
- Leaving an open lobby always asks first.

## Seats, leaving and returning

- **A player who leaves keeps the seat.** A deliberate leave is treated like a dropped connection: the AI plays the seat and
  `Rejoin Match` brings the player back while the match runs. The rejoin offer says so: "Your seat is held until the host
  gives it away."
- A newcomer who finds the match full can apply for a held seat. The host decides in the Players panel (F6), the lobby seat
  dialog or the lobby's Seats panel, all in the same words: `Keep for <player>`, `Let <newcomer> join`, `Cancel this
  approval`, and for a request `Decline` or `Ban`. In the Players panel `Let <newcomer> join`, `Decline` and `Ban` each ask
  for a second press that names the player ("Confirm: let <newcomer> join"). A declined newcomer reads "The host declined
  your request for this seat." and may ask again.
- The Players panel names why each seat is held ("Left 2 min ago", "Connection lost 10 s ago", "Machine too slow") and any
  pending request; a request waits 20 seconds for the host's answer. Nobody loses a seat without the host's click.
- Removing a player asks for a second press everywhere: the Players panel's `Remove` and `Ban`, the lobby seat dialog's
  `Kick player` and `Ban this session` ("Confirm kick" - "Press Confirm kick to remove <player> from this session"), and the
  session's `End session` ("Confirm end"). Any other press takes the first one back. A removed player stays refused for the
  rest of the match ("The host removed you from this session"). `Banned players` lists the session's bans; `Remove ban` lifts
  one for the host's next games and says so - a banned player stays out of the running match.

## In a match

- **The status box** says what is happening and who must act, before any number: whose brain is still being placed, who is
  away and why, how each player is connected. Its numbers (input delay, round trip, pace, waits) appear only with
  Settings > Network > Show network diagnostics.
- A player whose own seat is held reads "The AI is playing for you until you are back" once, in the status box; the
  notifications below never repeat a line the status box already shows, and the Players panel's line says how everyone
  else is ("Everyone else is playing").
- Under the default brain-loss rule ("Keep playing, humans spectate") a player whose brain is destroyed watches while the
  match plays on for everyone else; when only one side is left standing the round ends there.
- After a match the lobby's last-match line names the result and its reason in a player's words, such as "Joiner's brain was
  destroyed". `Details` shows the full record.

## Advanced (the host's pages)

- **Recovery:** autosave checkpoints are on by default, every 5 minutes, chosen from a drop-down of 1 to 60 minutes or turned
  off with the checkbox. The page says that held seats wait until the host gives them away. Automatic match repair is on.
- **Timing:** Input delay is Automatic by default. A hint under the rows names what the row under the hand costs: a higher
  minimum delay rides out spikes but every press lands later, a lower slow-player bound keeps everyone else smooth, more frame
  redundancy rides out lost packets but uses more bandwidth.
- **Connection:** the game listing (Public by default), `Ask my router to open the game port - players connect directly` (on
  by default), automatic connection and the match relay (the game service's by default).
- **Session:** the idle wait, the banned players list and `End session`.

## Settings > Network (this computer)

- **First page:** the multiplayer name, `Connection: Automatic` with `Change`, the chat switches, `Match status box` (When
  needed by default, with a one-line hint for each choice) and the connection indicator. `Advanced settings` opens the rest.
- **Hosting:** the defaults for games this computer hosts (input delay, idle lobby wait, path horizon, automatic repair), with
  a hint naming what the row under the hand costs.
- **In match:** chat, match notifications, Responsive controls ("Your own units answer at once; the match confirms each move a
  moment later") and Show network diagnostics.
- **Files:** autosave On or Off and its interval: in a match the match's own, labelled "Set by the host" ("Set by you as the
  host" for the host); outside one this computer's hosting default, labelled "Your hosting default". Then the autosaves kept,
  diagnostics and replays.
- **Connection:** Automatic (direct first, relay when needed), Direct only or Relay only, each with its cost; the STUN list
  and an optional fixed relay of the player's own.

## Joining

- When the directory asks the host to wait, Joining shows: "The directory is busy. The host retries in N seconds. Cancel
  returns to Multiplayer." The count follows the actual remaining wait. A pending seat stays reserved for that wait and
  admission resumes afterward.
- When the directory confirms that a match has ended or is gone, Joining offers another game. A host's own End Match shows the
  completed round's result, including "Match ended by host".
- A refused join says why in the player's words and what to do next. A session error the player cannot act on reads "The
  connection to the match failed - try again, or save a support report".
