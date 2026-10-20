#pragma once

// Discord rich presence over Discord's local IPC (discord.cpp): the game's presence line, the
// lobby's party size, and "Join" through the Steam lobby. No Discord SDK.
namespace xls {

// Starts the background connection (once; does nothing when "discord": {"enabled": false}).
void DiscordStart();
// Clears the activity and closes the connection.
void DiscordStop();

}
