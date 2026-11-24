# Changelog

Every release of MiusPlayer. Both players need the same network version; the launcher keeps the mod up to date.

## 2.8.6 - 2026-11-24 (network 31)
- Fixed: the main menu's MULTIPLAYER entry was gone after the player had been in an operation and left it - until the
  game was started again (and F9 and Steam's "Join Game" waited for a main menu that never came). The mod read the
  operational map's view to tell the main menu from the map, and the game leaves that at the map's last view; it now
  reads the game's own mode (menus or operation). The entry also no longer shows over the operational map when the
  map is in another view than its first (it could, on a map the game had opened straight after a battle).
- Nothing changes on the wire (network 31): players of 2.8.5 and 2.8.6 can play together.

## 2.8.5 - 2026-11-24 (network 31)
- Fixed: in an operation the turn did not go on when both players had pressed end turn - the joining player's game
  sent his moves only while his map was in its first view (the roster), and the game switches the map to the battle
  groups' view by itself when a player gives an order. Every view of the map counts now. (In every release since
  2.0; fixed in the development version after 2.8.1, released now.)
- Fixed: the two 1980 Khuzestan operations (Susangerd, Iran / Iraq) could not be shared - the joining player got
  the host's side of the map, his own side's units were left to the computer and the end of the turn hung. The mod
  now finds the Iran / Iraq operation of the other side as it does the German / Soviet one.
- The player panel (bottom left) can be docked to a small circle, out of the way of the game's own panels along the
  bottom of the screen: its small round button (lower right) docks it, a click on the circle shows it again, and
  F11 does either. It stays docked in the next battle and the next time the game starts.
- Nothing changes on the wire (network 31); both players should update for the two operation fixes.

## 2.8.4 - 2026-11-21 (network 31)
- Steam's new beta build, public_beta-26834468 of 2026-11-21. 2.8.3 was made for public_beta-26779327, which Steam
  no longer serves, so 2.8.3 keeps multiplayer off on the new build. This release is 2.8.3 with the mod moved to the
  new build and nothing else. The launcher recognises the new build and installs the mod for it; players on the two
  2024 builds can keep playing with 2.8.3.

## 2.8.3 - 2026-11-19 (network 31)
- Steam's new beta build, public_beta-26779327 of 2026-11-19. Steam's beta has changed five times since 2.8.2
  (which was made for public_beta-26665733 - Steam no longer serves that one, nor the four after it), so 2.8.2
  keeps multiplayer off there. This release is 2.8.2 with the mod moved to the new build and the one fix below. The
  launcher recognises the new build and installs the mod for it; players on the two 2024 builds can keep playing
  with 2.8.2.
- Fixed, on Steam's beta: in the joining player's game soldiers stood frozen, or moved their limbs in jerks - one
  pose a second - even with the camera right on them. The game works a figure's pose out more seldom the farther
  the camera is from him, and the mod took that from the host's game along with the rest of a soldier's animation:
  it was the host's camera that counted, wherever it stood. Each game now goes by its own camera. (In every release
  before, on that beta. Nothing changes on the wire - the network version stays 31.)

## 2.8.2 - 2026-11-14 (network 31)
- Steam's new beta build, public_beta-26665733 (it replaced public_beta-26514238, which Steam no longer serves).
  The launcher recognises it and installs the mod for it; players on the other builds can keep playing with 2.8.1.

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
