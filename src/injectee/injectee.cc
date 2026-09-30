// Fripack-compatible injectee using Chromatic engine
//
// This shared library is designed to be patched by the fripack CLI tool.
// It exports a `g_embedded_config` symbol that fripack patches with JS content.
// On load (constructor/DllMain), it reads the config, creates a Chromatic
// script runtime, and evaluates the embedded JS.

#include "config.h"
#include "core/script.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <fmt/core.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/// Global runtime instance (owned by the main thread)
std::unique_ptr<chromatic::script::runtime> g_runtime;

/// File watcher state
std::atomic<bool> g_stop_watching{false};
std::unique_ptr<std::thread> g_watch_thread;

/// Defined below, next to the platform entry points.
void shutdown_runtime();

std::string read_file(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    fmt::print(stderr, "[chromatic-injectee] Failed to open file: {}\n", path);
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

void start_file_watcher(const std::string &watch_path) {
  g_stop_watching = false;

  std::filesystem::file_time_type last_write_time;
  try {
    last_write_time = std::filesystem::last_write_time(watch_path);
  } catch (const std::exception &e) {
    fmt::print(stderr,
               "[chromatic-injectee] Failed to get initial file time: {}\n",
               e.what());
    return;
  }

  g_watch_thread = std::make_unique<std::thread>([watch_path,
                                                  last_write_time]() mutable {
    fmt::print("[chromatic-injectee] Watching file: {}\n", watch_path);

    while (!g_stop_watching) {
      try {
        auto current_time = std::filesystem::last_write_time(watch_path);

        if (current_time != last_write_time) {
          fmt::print(
              "[chromatic-injectee] File change detected, reloading...\n");
          last_write_time = current_time;

          // Small delay to let editors finish writing
          std::this_thread::sleep_for(std::chrono::milliseconds(50));

          std::string new_content = read_file(watch_path);
          if (!new_content.empty() && g_runtime) {
            // reset() calls on_dispose callbacks, then cleans up native hooks,
            // then reinitializes the JS engine
            g_runtime->reset();
            auto result = g_runtime->eval_script(new_content, watch_path);
            if (!result) {
              fmt::print(stderr,
                         "[chromatic-injectee] Script error on reload: {}\n",
                         result.error());
            } else {
              fmt::print("[chromatic-injectee] Script reloaded successfully\n");
            }
          }
        }
      } catch (const std::exception &e) {
        fmt::print(stderr, "[chromatic-injectee] Error watching file: {}\n",
                   e.what());
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    fmt::print("[chromatic-injectee] File watcher stopped\n");
  });
}

void injectee_main() {
  fmt::print(
      "[chromatic-injectee] Library loaded, initializing Chromatic engine\n");

  try {
    auto config = chromatic::injectee::parseEmbeddedConfig();

    g_runtime = std::make_unique<chromatic::script::runtime>();
    g_runtime->reset();

    // Tear down before the library's static destructors run, while the objects
    // the engine locks are still alive. See shutdown_runtime().
    std::atexit(shutdown_runtime);

    switch (config.mode) {
    case chromatic::injectee::EmbeddedConfigData::Mode::EmbedJs: {
      if (!config.js_content || config.js_content->empty()) {
        fmt::print(stderr,
                   "[chromatic-injectee] No JS content for EmbedJs mode\n");
        return;
      }

      auto result = g_runtime->eval_script(*config.js_content, "<embedded>");
      if (!result) {
        fmt::print(stderr, "[chromatic-injectee] Script error: {}\n",
                   result.error());
      }
      break;
    }

    case chromatic::injectee::EmbeddedConfigData::Mode::WatchPath: {
      if (!config.watch_path || config.watch_path->empty()) {
        fmt::print(stderr,
                   "[chromatic-injectee] No watch path for WatchPath mode\n");
        return;
      }

      std::string content = read_file(*config.watch_path);
      if (content.empty()) {
        fmt::print(stderr,
                   "[chromatic-injectee] Failed to read initial JS from: {}\n",
                   *config.watch_path);
        return;
      }

      auto result = g_runtime->eval_script(content, *config.watch_path);
      if (!result) {
        fmt::print(stderr, "[chromatic-injectee] Script error: {}\n",
                   result.error());
        return;
      }

      start_file_watcher(*config.watch_path);
      break;
    }

    default:
      fmt::print(stderr, "[chromatic-injectee] Unknown config mode: {}\n",
                 static_cast<int32_t>(config.mode));
      return;
    }

  } catch (const std::exception &e) {
    fmt::print(stderr, "[chromatic-injectee] Initialization failed: {}\n",
               e.what());
  }
}

} // namespace

// ─── Platform entry points ───

#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
  case DLL_PROCESS_ATTACH:
    std::thread([]() { injectee_main(); }).detach();
    break;
  case DLL_PROCESS_DETACH:
    g_stop_watching = true;
    if (g_watch_thread && g_watch_thread->joinable()) {
      g_watch_thread->join();
    }
    g_runtime.reset();
    break;
  }
  return TRUE;
}
#else
namespace {
/// Shuts the engine down while the process is still healthy.
///
/// This deliberately replaces the old `__attribute__((destructor))` teardown:
/// by the time library destructors run, the static objects the native subsystems
/// lock have already been destroyed, so tearing the runtime down there throws out
/// of a noexcept destructor and turns an ordinary exit into SIGABRT. Registering
/// the handler as soon as the runtime exists puts it ahead of those destructors,
/// because atexit handlers run in reverse order of registration.
void shutdown_runtime() {
  g_stop_watching = true;
  if (g_watch_thread && g_watch_thread->joinable()) {
    g_watch_thread->join();
  }
  g_runtime.reset();
}
} // namespace

/// Starts the injectee on its own thread.
///
/// The thread is not an implementation detail that can be folded away - the work
/// must not run inside this constructor. Doing it inline is tempting, and it does
/// fix one real symptom (a host that exits immediately never gives the thread a
/// chance to run the script at all), but it breaks the engine instead: the
/// constructor runs under the loader lock, and API calls that need the loader to be
/// free stop dead there. Measured with a three-line script, console.log only:
///
///     [probe] one
///     [probe] two
///     [probe] three
///
/// and the same script with Process.enumerateModules() inserted:
///
///     [probe] before enumerateModules
///
/// - the rest of the script never ran, and dlopen() returned as if all was well.
///
/// startup: spawn the worker, register the teardown from inside it (see
/// shutdown_runtime for why that ordering matters), and keep the constructor free of
/// engine work.
__attribute__((constructor)) static void _library_main() {
  std::thread([]() { injectee_main(); }).detach();
}
#endif
