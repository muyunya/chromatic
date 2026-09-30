// main.cc — gtest entry point + C function definitions
#include "test_common.h"
#include <cstdlib>
#include <cstring>

// ── Signal test skip flag ──
static bool g_skipSignalTests = false;
bool shouldSkipSignalTests() { return g_skipSignalTests; }

// ── C functions for hooking/calling from JS ──
// These are defined here (once) and declared extern in test_common.h
// noinline + optnone to ensure the functions have a proper prologue
// that the interceptor can disassemble and hook in release builds.

#ifdef __GNUC__
#define CHROMATIC_NOINLINE __attribute__((noinline, optnone))
#elif defined(_MSC_VER)
#define CHROMATIC_NOINLINE __declspec(noinline)
#else
#define CHROMATIC_NOINLINE
#endif

extern "C" CHROMATIC_NOINLINE int chromatic_test_add(int a, int b) {
  return a + b;
}
/// A hook target whose *first* instruction is a conditional branch.
///
/// Hand-written and naked so the prologue is exactly these instructions: a compiler
/// is free to emit anything, and the relocator's behaviour depends on what sits in
/// the first bytes of the function. See
/// Interceptor_RelocatesConditionalBranch for what goes wrong without the rewrite.
extern "C" __attribute__((naked, noinline)) int chromatic_test_guarded(int x) {
#if defined(__aarch64__)
  asm volatile("cbnz w0, 1f\n\t"
               "mov w0, #-1\n\t"
               "ret\n\t"
               "1:\n\t"
               "add w0, w0, #1\n\t"
               "ret\n\t");
#else
  asm volatile("test %rdi, %rdi\n\t"
               "jne 1f\n\t"
               "mov $-1, %eax\n\t"
               "ret\n\t"
               "1:\n\t"
               "lea 1(%rdi), %eax\n\t"
               "ret\n\t");
#endif
}
extern "C" CHROMATIC_NOINLINE int chromatic_test_mul(int a, int b) {
  return a * b;
}
extern "C" CHROMATIC_NOINLINE int chromatic_test_sub(int a, int b) {
  return a - b;
}

static int g_side_effect = 0;
extern "C" void chromatic_test_set_global(int v) { g_side_effect = v; }
extern "C" int chromatic_test_get_global() { return g_side_effect; }

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  // Parse --no-signal-tests flag
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--no-signal-tests") == 0) {
      g_skipSignalTests = true;
    }
  }

  // Also check environment variable
  if (auto *env = std::getenv("CHROMATIC_NO_SIGNAL_TESTS")) {
    if (std::strcmp(env, "1") == 0)
      g_skipSignalTests = true;
  }

  return RUN_ALL_TESTS();
}
