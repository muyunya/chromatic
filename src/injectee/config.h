#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace chromatic::injectee {

/// Fripack-compatible embedded config data.
/// This struct is deserialized from JSON embedded in the binary.
struct EmbeddedConfigData {
  enum class Mode : int32_t {
    EmbedJs = 1,   // JS content is directly embedded in the binary
    WatchPath = 2, // Read JS from a file path and watch for changes
  } mode;

  std::optional<std::string> js_filepath; // Unused (fripack legacy field)
  std::optional<std::string> js_content;  // EmbedJs mode: embedded JS source
  std::optional<std::string> watch_path;  // WatchPath mode: file path to watch
};

/// Binary header embedded in the shared library.
/// fripack patches this structure after compilation to embed data.
#pragma pack(push, 1)
struct EmbeddedConfig {
  int32_t magic1 = 0x0d000721;
  int32_t magic2 = 0x1f8a4e2b;
  int32_t version = 1;

  int32_t data_size = 0;
  int32_t data_offset = 0; // Offset from the start of the struct.
  bool data_xz = false;    // Whether the data is compressed with xz.
};
#pragma pack(pop)

/// Parse the embedded config from g_embedded_config.
/// Handles magic validation, xz decompression, and JSON deserialization.
EmbeddedConfigData parseEmbeddedConfig();

#ifdef __APPLE__
// ─── fripack payload buffer (Mach-O only) ─────────────────────────────────────
//
// On Mach-O, fripack writes the embedded script straight into the payload
// binary instead of rewriting the Mach-O structure, so the buffer it writes
// into has to exist in the file up front. fripack locates it by section name and
// then fills in `data_size` / `data_offset`.
//
// Two things are load-bearing:
//   * the buffer must be *file-backed*. The initialiser in config.cc is not
//     decoration: without it the linker folds the buffer into zerofill, it takes
//     no space in the file, and fripack has nowhere to write.
//   * it has to land in the same segment as `g_embedded_config` (__DATA),
//     because `data_offset` is a virtual-address delta between the two.
//
// ELF and PE do not need this: fripack appends a section to those instead.
//
// The buffer itself is defined in config.cc, where the section and visibility
// attributes live.
#ifndef CHROMATIC_FRIPACK_RESERVE
#define CHROMATIC_FRIPACK_RESERVE (1024 * 1024)
#endif
#endif

} // namespace chromatic::injectee
