#pragma once

#include <filesystem>
#include <memory>

#include "netplay/online_game_socket.h"
#include "online/online_service.h"

namespace tmnt {

// Called when the launcher opens: the service then knows the user folder.
void ConfigureOnline(const std::filesystem::path& user_dir);

// The process-wide service. Valid only after ConfigureOnline; it outlives the launcher so a lobby survives Play.
online::OnlineService& Online();

// The launcher's edited (not yet saved) settings, applied right away.
void TickOnline(const online::UserSettings& settings);

// Once per presented frame while the game runs, with the saved settings. A no-op until ConfigureOnline and after
// StopOnline.
void PumpOnlineService();

// Online play is on and the service is configured and not stopped: only then may the game's hooks use Online().
bool OnlineRunning();

// Clean shutdown: leaves any match and closes the service connection; the service is not pumped again.
void StopOnline();

double OnlineNowSeconds();

// The online gameplay UDP socket, bound as soon as Online is on and the game runs (well before any online menu), so
// Windows Firewall can prompt for network access right away. It then stays bound for the whole process: every match
// learns its public endpoint from it and plays over it. False while the bind fails (retried every few seconds).
bool EnsureOnlineGameSocket();
netplay::OnlineGameSocket& GameSocket();
}  // namespace tmnt
