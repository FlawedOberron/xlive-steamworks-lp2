Lost Planet 2 (Recolonised) - test build @DATE@ (retail DVD)

INSTALL (retail disc / DVD; needs Steam installed and signed in, you don't need to own LP2 on Steam)
1. Install the game from the disc (Linux: run setup.exe through Wine or Proton).
2. Unzip everything in this zip into the install folder (default C:\Program Files (x86)\Capcom\Lost Planet 2).
   It is already set up for the disc version (Spacewar, app 480: steam_appid.txt and xlive_steamworks.json).
3. Steam -> Add a Game -> Add a Non-Steam Game -> LP2DX9.exe or LP2DX11.exe from that folder.
4. Linux / Steam Deck: shortcut Properties -> Compatibility -> Proton Experimental, and launch option
   WINE_LARGE_ADDRESS_AWARE=1 %command%
5. Start it from Steam (Steam shows "Spacewar": that's expected). Plays online with Steam copies.
Have the Steam version instead? Use lp2-recolonised-test.zip.

Updates: from this build on, the game checks for a newer test build when it starts, downloads it and
says "Update downloaded ... restart the game to install it". Turn off with "update": {"auto": false}
in xlive_steamworks.json.

Discord: shows "Playing Lost Planet 2 (Recolonised)" automatically while Discord is running.
Problems: open an issue at https://github.com/FlawedOberron/xlive-steamworks-lp2/issues
          and attach xlive_steamworks.log from the game folder.

If the game crashes (report it at https://github.com/FlawedOberron/xlive-steamworks-lp2/issues ):
1. Don't worry about relaunching first: the log of the session before is kept as
   xlive_steamworks.prev.log (only one back, so grab it before launching a second time).
2. Send xlive_steamworks.log (the session that crashed, if you haven't relaunched) or
   xlive_steamworks.prev.log (if you have), from the game folder. The crash details
   (address and call stack) are at the end of it.
3. Say what you were doing: the menu or map, game mode, hosting or joining, and whether a
   map or custom part set was downloading.
4. Everyone in a match should be on the same test build; say which one you have.

Undo: delete the six files from this zip (README.txt, steam_api.dll, steam_appid.txt, xlive.dll,
xlive_relay.exe, xlive_steamworks.json). The GFWL xlive.dll in Windows is used again.
