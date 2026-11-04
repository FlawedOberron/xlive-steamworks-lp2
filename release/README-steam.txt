Lost Planet 2 (Recolonised) - test build @DATE@ (Steam copy)

INSTALL (Steam copy of Lost Planet 2)
1. Steam -> Lost Planet 2 -> Manage -> Browse local files.
2. Unzip everything in this zip into that folder and replace xlive.dll when asked.
3. Linux / Steam Deck: set launch option  WINE_LARGE_ADDRESS_AWARE=1 %command%
4. Play either the DirectX 9 or DirectX 11 version. Online = Steam lobbies (invite through Steam or Discord),
   shared with retail DVD copies: xlive_relay.exe joins them through Spacewar (Steam app 480) for you.
Have the retail DVD instead? Use lp2-recolonised-test-retail.zip.

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

Undo: delete xlive_relay.exe, steam_api.dll and xlive_steamworks.json, then Steam -> Lost Planet 2 ->
Properties -> Installed Files -> "Verify integrity of game files" (puts Capcom's xlive.dll back).
