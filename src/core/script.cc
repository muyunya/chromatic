#include "script.h"
#include "bindings/generated_bindings/binding_qjs.h"
#include "bindings/native_breakpoint.h"
#include "bindings/native_exception_handler.h"
#include "bindings/native_hw_breakpoint.h"
#include "bindings/native_interceptor.h"
#include "bindings/native_memory_access_monitor.h"
#include "bindings/script_lifecycle.h"
#include "fmt/base.h"

extern "C" {
extern const uint8_t _binary_index_js_start[];
extern const uint8_t _binary_index_js_end[];
}

std::string index_js = {(const char *)_binary_index_js_start,
                        (const char *)_binary_index_js_end};

namespace chromatic::script {
namespace {
/// Runs one teardown step, swallowing failures.
///
/// cleanup() is reached from ~runtime(), which is implicitly noexcept: an
/// exception escaping it calls std::terminate and takes the host process down
/// with it. That is not hypothetical - on macOS the native subsystems below lock
/// file-scope std::mutex objects, and when teardown happens late enough (from a
/// static destructor) those mutexes are already destroyed, so locking them
/// throws std::system_error("mutex lock failed: Invalid argument").
///
/// Skipping a teardown step is harmless while a process is exiting, so log and
/// carry on rather than aborting someone else's application.
template <typename F> void cleanup_step(const char *what, F &&step) {
  try {
    step();
  } catch (const std::exception &e) {
    fmt::print(stderr, "[chromatic] teardown step '{}' failed: {}\n", what,
               e.what());
  } catch (...) {
    fmt::print(stderr, "[chromatic] teardown step '{}' failed\n", what);
  }
}
} // namespace

void runtime::cleanup() {
  // Auto-cleanup all subsystems when the script context is disposed
  cleanup_step("memory access monitors",
               [] { chromatic::js::NativeMemoryAccessMonitor::disableAll(); });
  cleanup_step("hardware breakpoints",
               [] { chromatic::js::NativeHardwareBreakpoint::removeAll(); });
  cleanup_step("software breakpoints",
               [] { chromatic::js::NativeSoftwareBreakpoint::removeAll(); });
  cleanup_step("interceptors",
               [] { chromatic::js::NativeInterceptor::detachAll(); });
  cleanup_step("exception callbacks", [] {
    chromatic::js::NativeExceptionHandler::removeAllCallbacks();
  });
  cleanup_step("exception handler",
               [] { chromatic::js::NativeExceptionHandler::disable(); });
}
void runtime::reset() {
  // Let JS do its cleanup first via dispose callbacks
  chromatic::js::ScriptLifecycle::_callDisposeCallbacks();
  context.stop_event_loop_in_time(std::chrono::milliseconds(100));
  // Then native cleanup
  cleanup();
  context.on_bind.clear();
  context.on_bind.push_back(
      [this]() { chromatic_bindAll(context.js->addModule("chromatic")); });
  context.reset_runtime();
  if (auto res = context.eval_string(index_js, "<index>"); !res) {
    fmt::print("Failed to eval index.js: {}\n", res.error());
    return;
  }
}
std::expected<qjs::Value, std::string>
runtime::eval_script(const std::string &script, std::string_view filename) {
  return context.eval_string(script, filename);
}
} // namespace chromatic::script
