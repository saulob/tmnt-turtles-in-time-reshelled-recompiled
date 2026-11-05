// Audio client teardown: AudioSystem's dispatch against UnregisterClient (patch 0086), run on real threads without a
// runtime, next to the stock order the patch replaces, with checks that the patch still holds the modelled order.
// A release in this model zeroes the wrapper at once: the worst case of its page being reused.
// Run: tmnt_audio_client_teardown_tests

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kCallback = 0x82721BC0;
constexpr uint32_t kClientObject = 0x40E8E940;
constexpr auto kModelTimeout = 40ms;

class Latch {
 public:
  void Open() {
    std::lock_guard<std::mutex> guard(mutex_);
    open_ = true;
    changed_.notify_all();
  }

  void Wait() {
    std::unique_lock<std::mutex> guard(mutex_);
    changed_.wait(guard, [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool open_ = false;
};

struct Audio {
  std::recursive_mutex lock;
  uint32_t callback = kCallback;
  uint32_t wrapped_arg = 0x7006A000;
  std::atomic<uint32_t> wrapper_page{kClientObject};
  std::atomic<bool> driver{true};
  std::atomic<bool> running{false};
  std::atomic<std::thread::id> worker{};
  std::atomic<int> callbacks_run{0};
  std::atomic<int> dead_client_uses{0};
  std::chrono::milliseconds timeout = kModelTimeout;

  // What the game's callback touches: the arg through the wrapper, then the driver when it submits.
  void RunCallback(const std::function<void()>& body) {
    ++callbacks_run;
    if (wrapper_page.load() != kClientObject) ++dead_client_uses;
    if (body) body();
    if (wrapper_page.load() != kClientObject || !driver.load()) ++dead_client_uses;
  }

  void Release() {
    driver = false;
    wrapper_page = 0;
    std::lock_guard<std::recursive_mutex> guard(lock);
    wrapped_arg = 0;
  }

  void StockDispatch(const std::function<void()>& gap, const std::function<void()>& body) {
    {
      std::lock_guard<std::recursive_mutex> guard(lock);
      if (!callback) return;
    }
    if (gap) gap();
    RunCallback(body);
  }

  void StockUnregister() {
    {
      std::lock_guard<std::recursive_mutex> guard(lock);
      callback = 0;
    }
    Release();
  }

  void PatchedDispatch(const std::function<void()>& gap, const std::function<void()>& body) {
    uint32_t seen_callback;
    uint32_t seen_arg;
    {
      std::lock_guard<std::recursive_mutex> guard(lock);
      seen_callback = callback;
      seen_arg = wrapped_arg;
    }
    if (!seen_callback) return;
    if (gap) gap();
    {
      std::lock_guard<std::recursive_mutex> guard(lock);
      if (callback != seen_callback || wrapped_arg != seen_arg) return;
      running = true;
    }
    RunCallback(body);
    running = false;
  }

  bool WaitForCallbackExit() {
    if (std::this_thread::get_id() == worker.load()) return true;
    const auto deadline = Clock::now() + timeout;
    while (running.load()) {
      if (Clock::now() >= deadline) return false;
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  bool PatchedUnregister() {
    std::unique_lock<std::recursive_mutex> guard(lock);
    callback = 0;
    guard.unlock();
    const bool exited = WaitForCallbackExit();
    guard.lock();
    Release();
    return exited;
  }
};

void TheStockOrderHandsTheGameAReleasedClient() {
  Audio audio;
  audio.StockDispatch([&] { audio.StockUnregister(); }, nullptr);
  CHECK(audio.callbacks_run == 1);
  CHECK(audio.dead_client_uses > 0);
}

void AClientRemovedDuringThePacingGapIsNotDispatched() {
  Audio audio;
  audio.PatchedDispatch([&] { audio.PatchedUnregister(); }, nullptr);
  CHECK(audio.callbacks_run == 0);
  CHECK(audio.dead_client_uses == 0);
}

void AClientReplacedDuringThePacingGapIsNotDispatchedWithTheOldArg() {
  Audio audio;
  audio.PatchedDispatch(
      [&] {
        audio.PatchedUnregister();
        std::lock_guard<std::recursive_mutex> guard(audio.lock);
        audio.callback = kCallback;
        audio.wrapped_arg = 0x7006B000;
      },
      nullptr);
  CHECK(audio.callbacks_run == 0);
}

void ALiveClientIsDispatched() {
  Audio audio;
  audio.PatchedDispatch(nullptr, nullptr);
  CHECK(audio.callbacks_run == 1);
  CHECK(audio.dead_client_uses == 0);
  CHECK(!audio.running);
}

void UnregisterBlocksNewDispatchWhileItWaits() {
  Audio audio;
  audio.timeout = 5s;
  Latch started;
  Latch finish;
  std::atomic<bool> unregistered{false};
  std::thread worker([&] { audio.PatchedDispatch(nullptr, [&] { started.Open(); finish.Wait(); }); });
  started.Wait();
  std::thread remover([&] { audio.PatchedUnregister(); unregistered = true; });
  while (audio.callback != 0) std::this_thread::sleep_for(1ms);
  CHECK(!unregistered);
  std::atomic<int> before{audio.callbacks_run.load()};
  audio.PatchedDispatch(nullptr, nullptr);
  CHECK(audio.callbacks_run == before);
  finish.Open();
  worker.join();
  remover.join();
  CHECK(unregistered);
}

void UnregisterWaitsForARunningCallbackBeforeReleasing() {
  Audio audio;
  audio.timeout = 5s;
  Latch started;
  Latch finish;
  std::atomic<bool> returned{false};
  std::thread worker([&] { audio.PatchedDispatch(nullptr, [&] { started.Open(); finish.Wait(); }); });
  started.Wait();
  std::thread remover([&] { CHECK(audio.PatchedUnregister()); returned = true; });
  std::this_thread::sleep_for(30ms);
  CHECK(!returned);
  CHECK(audio.driver);
  CHECK(audio.wrapper_page == kClientObject);
  finish.Open();
  worker.join();
  remover.join();
  CHECK(returned);
  CHECK(!audio.driver);
  CHECK(audio.wrapper_page == 0);
  CHECK(audio.dead_client_uses == 0);
}

void ACallbackUnregisteringItsOwnClientDoesNotWaitForItself() {
  Audio audio;
  audio.timeout = 5s;
  std::atomic<bool> exited{false};
  const auto start = Clock::now();
  std::thread worker([&] {
    audio.worker = std::this_thread::get_id();
    audio.PatchedDispatch(nullptr, [&] { exited = audio.PatchedUnregister(); });
  });
  worker.join();
  CHECK(exited);
  CHECK(Clock::now() - start < 2s);
}

void AStuckCallbackCannotHangUnregister() {
  Audio audio;
  Latch started;
  Latch finish;
  std::thread worker([&] { audio.PatchedDispatch(nullptr, [&] { started.Open(); finish.Wait(); }); });
  started.Wait();
  const auto start = Clock::now();
  const bool exited = audio.PatchedUnregister();
  const auto elapsed = Clock::now() - start;
  CHECK(!exited);
  CHECK(elapsed >= kModelTimeout);
  CHECK(elapsed < 2s);
  CHECK(!audio.driver);
  finish.Open();
  worker.join();
}

void TheWrapperOutlivesEveryCallbackUnderRacingUnregisters() {
  for (int round = 0; round < 400; ++round) {
    Audio audio;
    audio.timeout = 5s;
    Latch go;
    std::thread worker([&] {
      go.Wait();
      for (int i = 0; i < 4; ++i) {
        audio.PatchedDispatch([&] { std::this_thread::yield(); }, [&] { std::this_thread::yield(); });
      }
    });
    std::thread remover([&] {
      go.Wait();
      std::this_thread::sleep_for(std::chrono::microseconds(round % 40));
      audio.PatchedUnregister();
    });
    go.Open();
    worker.join();
    remover.join();
    CHECK(audio.dead_client_uses == 0);
    CHECK(audio.wrapper_page == 0);
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  std::string normalized;
  for (char c : text.str()) {
    if (c != '\r') normalized.push_back(c);
  }
  return normalized;
}

bool InOrder(const std::string& text, std::initializer_list<const char*> needles) {
  size_t at = 0;
  for (const char* needle : needles) {
    at = text.find(needle, at);
    if (at == std::string::npos) {
      std::printf("  missing or out of order: %s\n", needle);
      return false;
    }
    at += std::string(needle).size();
  }
  return true;
}

void ThePatchKeepsTheModelledOrder() {
  const std::string patch = ReadFile(std::string(TMNT_PATCH_DIR) + "/0086-audio-unregister-waits-for-callback.patch");
  CHECK(!patch.empty());
  CHECK(InOrder(patch, {"+constexpr std::chrono::milliseconds kCallbackExitTimeout{500};",
                        "+  if (std::this_thread::get_id() == g_worker_thread.load()) {",
                        "+    return true;",
                        "+  const auto deadline = std::chrono::steady_clock::now() + kCallbackExitTimeout;",
                        "+  while (g_running_client.load() == index) {",
                        "+      return false;"}));
  CHECK(InOrder(patch, {"+          auto dispatch_lock = global_critical_region_.Acquire();",
                        "+          if (clients_[index].callback != client_callback ||",
                        "+              clients_[index].wrapped_callback_arg != client_callback_arg) {",
                        "+            continue;",
                        "+          g_running_client = static_cast<int>(index);",
                        " function_dispatcher_->Execute(",
                        "+        g_running_client = kNoClient;"}));
  CHECK(InOrder(patch, {"+  clients_[index].callback = 0;",
                        "+  global_lock.unlock();",
                        "+  const bool exited = WaitForCallbackExit(static_cast<int>(index));",
                        "+  global_lock.lock();",
                        " DestroyDriver(clients_[index].driver);",
                        " memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);"}));
}

}  // namespace

int main() {
  TheStockOrderHandsTheGameAReleasedClient();
  AClientRemovedDuringThePacingGapIsNotDispatched();
  AClientReplacedDuringThePacingGapIsNotDispatchedWithTheOldArg();
  ALiveClientIsDispatched();
  UnregisterBlocksNewDispatchWhileItWaits();
  UnregisterWaitsForARunningCallbackBeforeReleasing();
  ACallbackUnregisteringItsOwnClientDoesNotWaitForItself();
  AStuckCallbackCannotHangUnregister();
  TheWrapperOutlivesEveryCallbackUnderRacingUnregisters();
  ThePatchKeepsTheModelledOrder();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
