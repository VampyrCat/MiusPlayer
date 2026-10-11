# MiusPlayer
An attempt to mod multiplayer into Graviteam Mius Front.

GRAVITEAM TACTICS: MIUS-FRONT - MULTIPLAYER  (mpmod1, test version)
For game version 6.00, build 3598 (the final Steam version). Two players: one hosts, one joins.
Both players need the same mpmod1: network version 12, module built 2026-10-11 22:54.

QUICK START
  1. Put the whole "mpmod1" folder into the game folder - the one that contains starter_x64.exe.
     (Steam: right-click the game in your library, Manage, Browse local files.)
  2. Close the game and double-click   mpmod1\Enable_Multiplayer.bat
  3. Start the game as usual (through Steam). The main menu has a new entry at the top: MULTIPLAYER.
  4. One player hosts:   MULTIPLAYER -> HOST A SKIRMISH (or A CUSTOM MISSION, or AN OPERATION).
     The other joins:    MULTIPLAYER -> JOIN A GAME, type the host's address, CONNECT, then READY.
     Which address to type, and the port the host needs open: see NETWORK below.
  Back to the original game at any time:   mpmod1\Disable_Multiplayer.bat

WHAT ENABLE AND DISABLE DO
  Enable_Multiplayer.bat copies five new files from mpmod1\files into the game. No file of the game is
  changed or replaced:

      bin\rel\x64\basic_network.codelib   the multiplayer module
      root\programs\opgame.progpack       menu / operational map program, with the multiplayer module added
      root\programs\tacgame.progpack      battle program, with the multiplayer module added
      steam_appid.txt                     lets the game start a second copy of itself (joining a battle)
      mp_join.exe                         optional: join from a command prompt

  Before it copies anything it checks that the game is version 6.00, build 3598, and says so if it is
  not. If the game already has a steam_appid.txt it is left as it is. An earlier version of this
  multiplayer in the game is replaced.
  Until you choose MULTIPLAYER in the main menu the game is the normal single-player game; nothing is
  sent or received.

  Disable_Multiplayer.bat deletes those files again. The game is then exactly the original. Your
  multiplayer operation saves in users\mp_saves stay.

  While multiplayer is enabled, mpmod1 keeps a few notes in mpmod1\state. So: to move to a newer mpmod1,
  run Disable_Multiplayer.bat, replace the folder, run Enable_Multiplayer.bat - and do not delete the
  folder while multiplayer is enabled (if you did: put an mpmod1 folder back and run
  Disable_Multiplayer.bat).

PLAY
  Start the game as usual (Steam has to be running, as for the normal game). The main menu has one more
  entry at the top, MULTIPLAYER. Until you use it the game is the normal single-player game.

      MULTIPLAYER -> HOST A SKIRMISH        opens the lobby as host of a skirmish
                  -> HOST A CUSTOM MISSION  opens the lobby as host of a mission: a prepared one or one
                                            made in the battle editor
                  -> HOST AN OPERATION      opens the lobby as host of an operation
                  -> JOIN A GAME            type the host's address (Ctrl+V pastes), CONNECT: the host's lobby

  The lobby shows both players by their profile names, which force each commands, and whether the joined
  player is ready.
      Host:    SET UP THE BATTLE / CHOOSE THE MISSION / CHOOSE THE OPERATION opens the game's own
               Skirmish, Missions or Operations screen; set everything up and start the battle there as
               usual. A line at the top of the screen keeps showing who has joined. F9 brings the lobby
               back. LEAVE THE LOBBY stops hosting.
      Mission: the Missions screen lists the prepared missions and the ones made in the battle editor.
               The host normally commands the mission's player force and the joined player its enemy
               force; SWAP SIDES turns that round. The mission itself (units, objectives, triggers)
               travels to the joined player with the battle, so only the host needs to have it. A mission
               that uses a map the other player does not have cannot work.
      Joined:  READY. When the host starts the battle, a ready player's game downloads it and enters it.
      Skirmish, either player:  SWAP SIDES. The host sets up both forces on the Skirmish screen ("own" and
               "enemy"); normally the host commands the own force and the joined player the enemy force,
               SWAP SIDES turns that round.
      Operation: the host plays the side of the operation he opens (to play the other side, open the
               operation from that side's entry); modifiers are set on the Operations screen as usual.
               Every time an operation battle starts, the operation's save - written by the game after the
               move and before the battle - is copied to users\mp_saves\<host>_vs_<player>. The operation
               lobby lists these and RESTORE puts one back, so the host can go on from it whatever happened
               to either game. (What RESTORE replaces is kept in users\mp_saves\_replaced.)

  On the operational map (operations). Both players have the map at the same time and each moves his own
  side; nothing has to be handed back and forth.
      - When the host is on the map and the other player is ready in the lobby, the other player's game
        opens the same operation from his side by itself: his own territory and battle groups, fog of war
        over the host's forces (the host's groups show only where they stand next to his, as unidentified
        contacts). He selects and orders his groups as in a single-player operation.
      - The turn ends when BOTH players have pressed the map's "next turn" control. There is no time
        limit: a player who has pressed waits for the other, and pressing again takes it back. The line at
        the top of the screen says who is waiting for whom.
      - Then the other player's orders are fetched and put into the host's operation, the host's map
        reloads twice (about 20 seconds in all) and the turn is carried out: both players' ordered groups
        move, and the computer plans nothing for the other player's side (it still moves computer-led
        allies of the host). Nothing is drawn over the map meanwhile, so the moves can be watched; the
        host's map takes no clicks until the turn has been carried out.
      - After the battles of a turn the host presses next turn once more (as in the single-player game)
        to move on; the next turn's map then goes to the other player.
      - The other player stays on the operational map throughout. When the turn has been carried out his
        map reloads with the result (one loading screen) and he gives the next turn's orders. If the turn
        leads to a battle, his game goes from the map straight into the battle as soon as the host starts
        it; after the battle it returns to the host by itself and gets the map of what follows.
      - If the other player's game goes away (closed, connection lost), the host is not kept waiting: his
        next-turn control ends the turn the usual way, without the other player.
  The other player's game keeps the host's operation in its own profile as that operation's save (for
  the other side's version of the operation); a save of his own with that name is kept in
  users\mp_saves\_replaced.
  A battle has to be fought to its end for the operation to move on to the next turn.

  In the battle. Each player commands one force; the computer commander is switched off. The line at
  the top of the screen always has the same shape:
      MULTIPLAYER  -  <you> (<your side>)  vs  <other player> (<their side>)  -  <what is going on>
  Deployment ends when BOTH players have pressed the game's Start button (the arrow in the top toolbar,
  or Enter), and the battle starts when both have pressed it again after the initial orders.
  The host controls the clock: only the host can pause or change the time speed. Both games simulate at
  the same fixed rate (50 steps per second), so a faster PC gives no advantage.
  The two battle clocks are kept together by catching up, never by dropping or redoing anything: a game
  that has fallen behind simulates faster (up to four times) until it is level, and the host slows down
  and, if need be, stops for a joined game that cannot keep up ("waiting for ...'s game to catch up" in
  the top line). Both games then run at the pace of the slower PC and neither misses anything.
  Fog of war is each game's own: a player sees the other player's units only while his own units have
  them spotted.
  If the host closes its game or the connection is lost, the joined game closes the battle after a few
  seconds and returns to the menu.

NETWORK
  The host needs UDP port 1943 reachable from the other player. On a home network the game asks the router
  for it (UPnP); if the router refuses, forward UDP 1943 to the host PC by hand. If Windows Firewall asks
  whether starter_x64.exe may communicate on the network when you host for the first time, allow it.
  Only the host needs the port; the joining player needs nothing set up.
      Over the internet:    the joining player types the host's public address
      On the same network:  the address shown in the host's lobby
      On the same PC:       127.0.0.1
  From a command prompt:    mp_join.exe <host address>[:port] [germans | soviets]
  Expect roughly 300-400 KB/s in each direction during a battle of about 150 units. The state stream is
  limited to 600 KB/s per game (MP_STATE_KBPS changes that).

HOW IT WORKS, AND ITS LIMITS
  Each PC simulates the whole battle. The game of the player who commands a unit sends that unit's
  position 15 times a second and its complete saved state whenever it changes; the other game loads them.
  Damage to a unit is therefore decided on its owner's PC.
  The other player's units are shown by interpolation: a tenth of a second in the past, moving smoothly
  from one received position to the next (they arrive about 15 times a second), instead of being nudged
  at every arrival with this game's own physics pushing them about in between. A unit whose owner has
  stopped sending (it stands still there) stays where the owner last had it. Nothing is predicted:
  every unit is simulated without delay by its owner's game.
  Trenches belong to the game that owns the squad: when a squad is dug in, what went into it (place,
  facing of its leading unit, kind of trench, dig-in settings) is sent and the other game digs from
  those values; what it would have dug for that squad by itself is dropped. MP_SYNC_TRENCH=0 switches
  this off, MP_LOG_TRENCH=1 logs every dig.
  Shots belong to the game that owns the firing unit. Every shell, bullet, smoke round and grenade it
  launches is sent to the other game, which launches it with exactly the same start, direction and speed;
  what the other game's own simulation would have fired for that unit is dropped. Launches are sent
  reliably and in order (repeated until acknowledged). MP_SYNC_SHOTS=0 switches this off, MP_LOG_SHOTS=1
  writes every launch to out\mp_shots_<pid>.txt.
  A saved state is only loaded into a unit whose internal render handles are the same in both games (they
  are when both games built the battle the same way, which is the normal case when the second player
  joins during deployment). Where they differ, that unit is followed by position only; the game's log
  (out\basic_network_tacgame_*.log) counts these as "not loaded".
  Not done yet:
    - operational map: the other side's view needs the game to have that operation from the other side
      (e.g. ..._rus and ..._ger); for an operation without one the other player sees the host's version,
      with all of the host's groups visible. What each player knows of the other's groups is not kept by
      the game for the second player. He is shown a group of the host's side, as an unidentified
      contact, where it stands next to one of his own groups that the host can see; after a shared turn
      the host is shown the other side's groups that stand next to his own. All sixteen operations of
      the game have their other-side version with the same formations.
      Played with both games on one PC: Odintsov (a turn and its battle) and Saur-Mogila (a turn, its
      battle to the end, the turn after it with both players' orders, into the next battle) - the battle's
      one-hour limit was shortened for that test; the other operations were checked from their files and
      saves, not played;
    - operation battles: units are followed by position only - losses and damage are worked out by each
      game for itself and will differ (loading the other game's unit states crashes the game in
      operation battles, so it is switched off there);
    - skirmish: both forces are chosen by the host on the Skirmish screen; the joined player cannot pick
      his own units from his game;
    - what a shot does when it lands is still worked out by each game: craters, the smoke cloud of a
      smoke round, destroyed buildings and the battle statistics can differ a little, and so can the
      moment the battle ends. Explosions that are not a launch (mines, ammunition going off) and off-map
      artillery called by the joined player are not sent yet. Flares have not come up in a test;
    - trenches: the digging the game does by itself while both players are connected comes out the same
      in both games (checked in a Saur-Mogila battle: all 154,042 pieces alike). A squad that its player
      puts somewhere else during deployment is dug in on the other PC too, but NOT in the same place yet
      (in the test none of its new pieces matched) - so a vehicle crossing such a trench can still be
      shown displaced there. Shell craters are made by each game for itself;
    - operation battles: a vehicle can be destroyed on one PC and only damaged on the other, and its gun
      can point differently (see "units are followed by position only" above);
    - unit states in operation battles: tried again and still off. With MP_SYNC_STATE=1 the host's game
      crashes as the battle starts (in the vehicle update, at adv_game+0xd9917), so there units stay
      followed by position only;
    - a soldier's animation is run by each game for itself: at times a running soldier is shown lying
      down and sliding on the other PC. In operation battles, where units are followed by position
      only, the other player's soldiers have no posture from their owner at all;
    - the crew of a few vehicles can be drawn in a different place on the other PC;
    - two players only.

  Running two copies on one PC (for testing): they share the graphics card. The copy that is not the
  active window is held to 30 frames per second and the active one gives way until the other reaches
  that. Environment variables, set before starting the game: MP_BG_FPS (background limit, 0 = none),
  MP_FG_FPS (fixed limit for the active window), MP_SIM_HZ (simulation steps per second, 0 = stock).
