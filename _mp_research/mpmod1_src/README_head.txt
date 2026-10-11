GRAVITEAM TACTICS: MIUS-FRONT - MULTIPLAYER  (mpmod1, test version)
For game version 6.00, build 3598 (the final Steam version). Two players: one hosts, one joins.
Both players need the same mpmod1: @VERSION@.

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

