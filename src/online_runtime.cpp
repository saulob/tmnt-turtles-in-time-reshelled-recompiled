#include "online_runtime.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "netplay/online_game_socket.h"
#include "netplay_setting.h"
#include "online/identity.h"
#include "online/instance.h"
#include "online/local_addresses.h"
#include "online_setting.h"
#include "player_match.h"
#include "port_version.h"

REXCVAR_DEFINE_BOOL(tmnt_online_enabled, tmnt::kDefaultOnlineEnabled, "TMNT",
                    "Online play: friends, and the game's online matches with them over the internet");
REXCVAR_DEFINE_STRING(tmnt_online_name, tmnt::kDefaultOnlineNameValue, "TMNT",
                      "Name other players see during online play");
REXCVAR_DEFINE_BOOL(tmnt_online_ping_display, tmnt::kDefaultOnlinePing, "TMNT",
                    "Shows the round trip to the other player during an online match");
REXCVAR_DEFINE_STRING(tmnt_online_connection_test, "", "TMNT",
                      "UDP configuration of the last Connection test finished with Online play on, written by the "
                      "launcher; empty: none since Online play was turned on");

namespace tmnt {
namespace {

// Declared before the service, which taps it, so it is destroyed after.
netplay::OnlineGameSocket g_game_socket;
double g_next_bind_try = 0;
bool g_bind_failure_logged = false;

std::mutex g_mutex;
std::filesystem::path g_user_dir;
bool g_configured = false;
bool g_stopped = false;
std::unique_ptr<online::OnlineService> g_service;
std::unique_ptr<online::InstanceLock> g_lock;
bool g_game_pumped = false;
bool g_socket_bound_in_game = false;
bool g_runtime_signed_in = false;

// After Play restarts the process, the launcher that holds the lock may still be exiting.
constexpr int kLockAttempts = 12;
constexpr auto kLockRetry = std::chrono::milliseconds(250);
constexpr double kBindRetrySeconds = 5.0;

online::Identity ResolveIdentity() {
  for (int attempt = 0; attempt < kLockAttempts && !g_lock; ++attempt) {
    if (attempt) std::this_thread::sleep_for(kLockRetry);
    g_lock = online::InstanceLock::TryAcquire(g_user_dir);
  }
  if (!g_lock) {
    REXLOG_INFO("DEBUG_SAULO online second instance on this user folder: temporary identity");
    return online::MakeEphemeralIdentity();
  }
  return online::LoadOrCreateIdentity(g_user_dir);
}

const char* PlatformName() {
#if defined(_WIN32)
  return "Windows";
#else
  return "Linux";
#endif
}

online::ServiceConfig MakeConfig() {
  online::ServiceConfig config;
  if (const char* url = std::getenv(kOnlineServiceEnv); url && *url) config.url = url;
  if (const char* stun = std::getenv(kStunServersEnv); stun && *stun) config.stun = stun;
  if (const char* upnp = std::getenv(kUpnpEnv); upnp && *upnp) config.upnp = std::string_view(upnp) != "0";
  config.guest = online::kGuestBase;
  config.build = online::kOnlineBuild;
  config.version = PortVersion();
  config.platform = PlatformName();
  config.game_ports.assign(std::begin(kGameplayPorts), std::end(kGameplayPorts));
  config.game_ports.push_back(0);
  config.lan = online::LocalCandidates();
  config.game_socket = &g_game_socket;
  config.identity = ResolveIdentity;
  config.log = [](const std::string& line) { REXLOG_INFO("{}", line); };
  config.invite_accepted = DeliverAcceptedInvite;
  return config;
}

online::UserSettings SavedSettings() {
  online::UserSettings settings;
  settings.enabled = REXCVAR_GET(tmnt_online_enabled);
  settings.name = REXCVAR_GET(tmnt_online_name);
  return settings;
}

}  // namespace

void ConfigureOnline(const std::filesystem::path& user_dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_user_dir = user_dir;
  g_configured = true;
}

online::OnlineService& Online() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_service) g_service = std::make_unique<online::OnlineService>(MakeConfig());
  return *g_service;
}

void TickOnline(const online::UserSettings& settings) {
  if (settings.enabled) EnsureOnlineGameSocket();
  online::OnlineService& service = Online();
  service.Apply(settings);
  service.Update(OnlineNowSeconds());
}

void PumpOnlineService() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_configured || g_stopped) return;
  }
  const online::UserSettings settings = SavedSettings();
  if (!g_game_pumped && settings.enabled) {
    g_game_pumped = true;
    REXLOG_INFO("DEBUG_SAULO online runtime reconnect begin guest={}", online::kGuestBase);
    REXLOG_INFO("DEBUG_SAULO udp socket at game start open={} bound_in_game={}", g_game_socket.is_open(),
                g_socket_bound_in_game);
  }
  TickOnline(settings);
  const bool signed_in = Online().SignedIn();
  if (signed_in != g_runtime_signed_in) {
    g_runtime_signed_in = signed_in;
    if (signed_in) REXLOG_INFO("DEBUG_SAULO online runtime connected");
    REXLOG_INFO("DEBUG_SAULO online runtime signed in={}", signed_in);
  }
}

bool OnlineRunning() {
  if (!REXCVAR_GET(tmnt_online_enabled)) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_configured && !g_stopped;
}

void StopOnline() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool running = !g_stopped && g_service;
    g_stopped = true;
    if (!running) return;
  }
  online::UserSettings off = SavedSettings();
  off.enabled = false;
  g_service->Apply(off);
  g_service->Update(OnlineNowSeconds());
  REXLOG_INFO("DEBUG_SAULO online runtime stopped");
}

double OnlineNowSeconds() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool EnsureOnlineGameSocket() {
  if (g_game_socket.is_open()) return true;
  const double now = OnlineNowSeconds();
  if (now < g_next_bind_try) return false;
  // A second instance on this PC finds the first port taken and takes the other; the advertised endpoint says which.
  if (g_game_socket.Open({kGameplayPorts[0], kGameplayPorts[1], kGameplayPorts[2], kGameplayPorts[3], 0})) {
    g_socket_bound_in_game = g_game_pumped;
    REXLOG_INFO("DEBUG_SAULO udp socket bound local=0.0.0.0:{} phase={}", g_game_socket.bound_port(),
                g_game_pumped ? "game" : "launcher");
    return true;
  }
  g_next_bind_try = now + kBindRetrySeconds;
  if (!g_bind_failure_logged) {
    g_bind_failure_logged = true;
    REXLOG_INFO("DEBUG_SAULO udp socket bind failed");
  }
  return false;
}

netplay::OnlineGameSocket& GameSocket() { return g_game_socket; }

}  // namespace tmnt
