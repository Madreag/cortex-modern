# Security

## Supported versions

Only the newest alpha receives fixes. Players on different versions cannot join each other, so updating is always the first step.

## Reporting a vulnerability

Please do not open a public issue for something that could be used against other players.

Use GitHub's private reporting instead: on this repository open **Security**, then **Report a vulnerability**. Describe what you did, what happened, the version line from the main menu, and your operating system. You will get an answer there.

## What counts

- Crashes or memory corruption caused by anything that arrives over the network: lobby and match messages, the session directory's replies, relay data.
- Crashes or code execution from loading a save, a checkpoint, a replay or a match snapshot received from another player.
- Ways to read or misuse another player's relay login, rejoin ticket or installation key.
- Ways to abuse the public session directory: listing floods, taking over someone else's listing, turning it against a third party.
- Ways to take another player's seat, or to kick or ban without being the host.

## What does not

This is peer-to-peer lockstep: every player's machine simulates the whole match and holds the whole game state. A modified client can therefore see everything in the match, and can desynchronize itself. That is how the design works and is not treated as a vulnerability. The game detects a desync and repairs it or holds the affected seat.

Mods run with the same access as the game itself. Only install mods you trust, as with the original game.
