#include "online_runtime.h"

#if !defined(_WIN32)
#include <csignal>
#endif

#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

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
REXCVAR_DEFINE_INT32(tmnt_online_party_snapshot, 0, "TMNT",
                     "Encoded turtles of the last match start, written by the character select hook in TMNT_r");

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
bool g_runtime_signed_in = false;

// After Play restarts the process, the launcher that holds the lock may still be exiting.
constexpr double kLockWaitSeconds = 2.75;
constexpr double kBindRetrySeconds = 5.0;

double g_lock_deadline = 0;

// Asked once per retry from the thread that presents, which must not sleep here.
online::Identity ResolveIdentity() {
  if (!g_lock) g_lock = online::InstanceLock::TryAcquire(g_user_dir);
  if (!g_lock) {
    const double now = OnlineNowSeconds();
    if (g_lock_deadline == 0) g_lock_deadline = now + kLockWaitSeconds;
    if (now < g_lock_deadline) {
      online::Identity pending;
      pending.pending = true;
      return pending;
    }
    REXLOG_INFO("online second instance on this user folder: temporary identity");
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

void ForwardPartySnapshot() {
  static int32_t forwarded = 0;
  const int32_t encoded = REXCVAR_GET(tmnt_online_party_snapshot);
  if (encoded == forwarded) return;
  forwarded = encoded;
  if (const std::optional<online::PartySnapshot> snapshot = online::DecodePartySnapshot(encoded)) {
    Online().NotePartySnapshot(*snapshot);
  }
}

online::UserSettings SavedSettings() {
  online::UserSettings settings;
  settings.enabled = REXCVAR_GET(tmnt_online_enabled);
  settings.name = REXCVAR_GET(tmnt_online_name);
  return settings;
}

}  // namespace

void ConfigureOnline(const std::filesystem::path& user_dir) {
#if !defined(_WIN32)
  // OpenSSL writes with plain write(), so a link the peer reset would otherwise end the process without a crash report.
  std::signal(SIGPIPE, SIG_IGN);
#endif
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
    REXLOG_INFO("online runtime reconnect begin guest={}", online::kGuestBase);
  }
  ForwardPartySnapshot();
  TickOnline(settings);
  const bool signed_in = Online().SignedIn();
  if (signed_in != g_runtime_signed_in) {
    g_runtime_signed_in = signed_in;
    if (signed_in) REXLOG_INFO("online runtime connected");
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
  REXLOG_INFO("online runtime stopped");
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
    REXLOG_INFO("udp socket bound local=0.0.0.0:{} phase={}", g_game_socket.bound_port(),
                g_game_pumped ? "game" : "launcher");
    return true;
  }
  g_next_bind_try = now + kBindRetrySeconds;
  if (!g_bind_failure_logged) {
    g_bind_failure_logged = true;
    REXLOG_INFO("udp socket bind failed");
  }
  return false;
}

netplay::OnlineGameSocket& GameSocket() { return g_game_socket; }

}  // namespace tmnt
