# Changelog

Every release of MiusPlayer. Both players need the same network version; the launcher keeps the mod up to date.

## 2.8.1 - 2026-11-09 (network 31)
- Fixed: guns and vehicles a player placed on the deployment screen could sink into the ground and be destroyed
  (or be thrown far away) once the battle had begun - in the host's game his units kept their wheels where they had
  stood before he moved them. Since 2.7.0; seen with anti-tank guns in a two-PC operation battle.
- Fixed: enemy soldiers that had just gone out of sight could move and animate in jerks (as if at 2 frames a second)
  on the joining player's screen - the host sent them as units nobody sees while his game still drew them. A unit
  now counts as seen for 20 s after the host's spotting last had it. (Since 2.8.0.)

## 2.8.0 - 2026-11-08 (network 31)
- Less traffic from the host in big battles (Stepanovka, 1280 units: 1.1 MB/s down to about 0.8 MB/s):
  - a moving unit is left out while the other game, running it on from what it last got, has it within a few centimetres;
  - in a versus battle, the host's units the other player has not spotted (not drawn in his game) move, change and
    animate less often on the wire until he spots them;
  - the full states of units standing still go every 1.6 s.
- The other player's soldiers are followed more smoothly: they travel with their real ground speed (far fewer shaking).
- The joining game spends less time on the other player's units (seats, conditions, animation).
- Joining an operation: the operational map (30-40 MB) arrives in about 20 s whatever the frame rate; before, it could
  take 50 s and the host's map went into its battle first - his battle stood at 97 % and the joining game waited for
  ever. If it happens all the same, the joining game now says so after 10 s.
- The player panel's status line and its Latency / FPS numbers change together, once a second.
- Fixed: the host's battle could crash a few seconds after a player connected (seen on Steam's 2026 beta).

## 2.7.1 - 2026-11-08 (network 30)
- Steam's new beta build, public_beta-26514238 (it replaced public_beta-26494568, which Steam no longer serves).

## 2.7.0 - 2026-11-08 (network 30)
- Big operation battles: units' movements take half the room on the wire; vehicles' wheels and tracks no longer jump
  when a state arrives; the other player's vehicles no longer shake in place or on the move; riders the host keeps
  on board stay seated.
- The player panel in the bottom-left corner replaces the line at the top of the screen: what is going on, Latency,
  FPS, what is being played ("Operation - Stepanovka - 1943"), and a click opens the list of players.
  Ctrl+Backspace hides it with the game's own interface.

## 2.6.0 - 2026-11-08 (network 29)
- Chat in a battle: Enter opens a line, Enter sends it to every player.
- The ping to the other game on screen.

## 2.5.0 - 2026-11-08 (network 28)
- Up to five players (the lobby's PLAYERS button).
- Poor connections: the host sends states only as fast as the line carries them.
- A connection lost in a battle is waited for up to 90 s, and the battle goes on where the host's game has it.
- Shorter lobby texts; faster.

## 2.4.0 - 2026-11-07 (network 27)
- A track breaking and its repair are the host's.

## 2.3.0 - 2026-11-07 (network 26)
- What each player sees of the enemy is the host's spotting.

## 2.2.1 - 2026-11-07
- Both players on the same side command the side's units (co-op orders were refused by the host).

## 2.2.0 - 2026-11-07
- The map's objects (houses, fences, trees) are damaged as the host's game has them.

## 2.1.0 - 2026-11-07 (network 24)
- Aim orders (sector, turn, fire at ground), who rides which vehicle, who holds the ground, running gear on Steam's
  2026 beta.

## 2.0.0 - 2026-11-07
- The game's Steam default and beta builds; a launcher that installs the mod and keeps it up to date; releases built
  by GitHub Actions.
