# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org)

## What is signed

`MiusPlayerLauncher.exe` of each release. It is built by GitHub Actions on GitHub's own Windows machines from the
source code in this repository (`launcher/`, built by `launcher/build_launcher.bat` in
`.github/workflows/release.yml`), sent to SignPath from that workflow, and signed only after a maintainer has
approved the release's signing request. Nothing built anywhere else is signed.

## Team roles

- Committers and reviewers: [VampyrCat](https://github.com/VampyrCat)
- Approvers: [VampyrCat](https://github.com/VampyrCat)

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the
user or the person installing or operating it. In detail:

- The launcher asks GitHub (api.github.com, github.com) for the latest release of this project and downloads it,
  when it is opened or told to install or update the mod. GitHub sees the request as for any download.
- The mod connects to another player's game only when the player hosts a multiplayer game or joins one (by
  address over UDP, or through Steam when joining a Steam friend). It then sends what the shared battle needs: the
  game's state, the player's profile name and the chat lines the player types.
- When hosting with "Open it by UPnP" ticked, the mod asks the player's own router to open the game's port.
- When hosting through Steam, Steam's friends list shows the player's friends that the game can be joined.

Nothing else is collected or sent anywhere.

## Installing and uninstalling

The launcher's Install adds the mod's files to the game folder and keeps a list of them; Uninstall removes them and
puts back what was changed, leaving the game as it was. Outside the game folder the launcher writes only its own
settings file (`%APPDATA%\MiusPlayer\launcher.json`), its downloads while they are being made (in the temporary
folder), and - when a release has a newer launcher - the new launcher in place of itself.
