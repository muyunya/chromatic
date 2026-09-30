#include "native_process.h"
#include "native_pointer.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

#ifdef CHROMATIC_WINDOWS
#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef CHROMATIC_DARWIN
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#endif

#if defined(CHROMATIC_LINUX) || defined(CHROMATIC_ANDROID)
#include <elf.h>
#include <fstream>
#include <link.h>
#endif

namespace chromatic::js {

#ifdef CHROMATIC_DARWIN
namespace {

/// Where a Mach-O image actually lives in memory.
struct MachoImageLayout {
  /// Value for ModuleInfo::size: the span of the run of segments that is
  /// contiguous with the image header, i.e. the mapping the header lives in.
  int size = 0;
  /// The pieces that make up the image, for callers that need all of it rather
  /// than just the header's mapping.
  std::vector<SegmentInfo> regions;
};

/// Describes a Mach-O image.
///
/// The old code took the highest `vmaddr + vmsize` over the segments, which is
/// an *absolute address*, not a length. Truncated into the `int` that
/// ModuleInfo stores, it wrapped around: libSystem.B.dylib was reported as 627 MB
/// instead of 7 KB, and `Memory.scanModule` then walked a 598 MB range that
/// contained 101 other modules - 923 ms instead of 0 ms, and matches attributed
/// to the wrong library.
///
/// A single contiguous size is not enough either, because an image in the dyld
/// shared cache is scattered across it: measured for libSystem.B.dylib, __TEXT
/// sits at 0x18e6b5000, __DATA_CONST at 0x1e87364d8 and __AUTH_CONST at
/// 0x1f06f69a8. So `size` describes the header's mapping, and `regions` lists
/// every piece that belongs to the image.
///
/// Two segments are deliberately left out of `regions`:
///
///   * __PAGEZERO, an unmapped 4 GB guard region rather than image content -
///     including it would put the image's start address at 0;
/// `slide` is the image's ASLR offset: segment vmaddrs are the link-time
/// addresses, so every region has to be reported at vmaddr + slide or it points
/// at unmapped memory.
///
///   * a __LINKEDIT that does not sit with the rest of the image. In the shared
///     cache that segment belongs to the cache, not to the image: libSystem and
///     libc++abi both report the identical range 0x1fee98000 + 0x26760000
///     (617 MB). It holds symbol tables and rebase metadata rather than anything
///     worth pattern-scanning, and scanning it would walk most of the cache. A
///     __LINKEDIT that does sit with its image is ordinary image content and is
///     kept.
MachoImageLayout describeMachoImage(const struct mach_header_64 *header,
                                    intptr_t slide) {
  struct Segment {
    uint64_t start;
    uint64_t size;
    bool isLinkEdit;
  };

  std::vector<Segment> segments;
  segments.reserve(header->ncmds);

  auto cmd = reinterpret_cast<const struct load_command *>(header + 1);
  for (uint32_t i = 0; i < header->ncmds; i++) {
    if (cmd->cmd == LC_SEGMENT_64) {
      auto seg = reinterpret_cast<const struct segment_command_64 *>(cmd);
      const bool isPageZero = std::strncmp(seg->segname, "__PAGEZERO", 10) == 0;
      const bool isLinkEdit = std::strncmp(seg->segname, "__LINKEDIT", 10) == 0;
      if (!isPageZero && seg->vmsize > 0)
        segments.push_back(
            {seg->vmaddr + static_cast<uint64_t>(slide), seg->vmsize, isLinkEdit});
    }
    cmd = reinterpret_cast<const struct load_command *>(
        reinterpret_cast<const uint8_t *>(cmd) + cmd->cmdsize);
  }
  if (segments.empty())
    return {};

  std::sort(segments.begin(), segments.end(),
            [](const Segment &a, const Segment &b) { return a.start < b.start; });

  // Segments are page aligned, so treat anything within a page as contiguous.
  // 16 KiB covers both arm64 and x86_64 page sizes.
  constexpr uint64_t kPageMask = 0x3fff;
  const auto touches = [](uint64_t end, uint64_t next) {
    return next <= ((end + kPageMask) & ~kPageMask);
  };
  const auto segmentEnd = [](const Segment &s) { return s.start + s.size; };

  // The run starting at the header decides `size`.
  uint64_t runEnd = segmentEnd(segments[0]);
  size_t runLength = 1;
  for (; runLength < segments.size(); runLength++) {
    if (!touches(runEnd, segments[runLength].start))
      break;
    runEnd = std::max(runEnd, segmentEnd(segments[runLength]));
  }

  constexpr uint64_t kIntMax = 0x7fffffffULL;
  MachoImageLayout layout;
  layout.size = static_cast<int>(
      std::min<uint64_t>(runEnd - segments[0].start, kIntMax));

  for (size_t i = 0; i < segments.size(); i++) {
    const Segment &segment = segments[i];
    const bool inRun = i < runLength;
    if (!inRun && segment.isLinkEdit)
      continue; // the shared cache's, not this image's (see above)

    if (!layout.regions.empty()) {
      SegmentInfo &last = layout.regions.back();
      if (touches(last.base + last.size, segment.start)) {
        last.size = std::max(last.base + last.size, segmentEnd(segment)) - last.base;
        continue;
      }
    }
    layout.regions.push_back(SegmentInfo{segment.start, segment.size});
  }

  return layout;
}

} // namespace
#endif

std::string NativeProcess::getArchitecture() {
#ifdef CHROMATIC_ARM64
  return "arm64";
#elif defined(CHROMATIC_X64)
  return "x64";
#else
  return "unknown";
#endif
}

std::string NativeProcess::getPlatform() {
#ifdef CHROMATIC_WINDOWS
  return "windows";
#elif defined(CHROMATIC_ANDROID)
  return "android";
#elif defined(CHROMATIC_LINUX)
  return "linux";
#elif defined(CHROMATIC_DARWIN)
  return "darwin";
#else
  return "unknown";
#endif
}

int NativeProcess::getPointerSize() { return sizeof(void *); }

int NativeProcess::getPageSize() {
#ifdef CHROMATIC_WINDOWS
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return static_cast<int>(si.dwPageSize);
#else
  return static_cast<int>(sysconf(_SC_PAGESIZE));
#endif
}

int NativeProcess::getProcessId() {
#ifdef CHROMATIC_WINDOWS
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}

std::shared_ptr<NativePointer> NativeProcess::getCurrentThreadId() {
#ifdef CHROMATIC_WINDOWS
  return std::make_shared<NativePointer>(static_cast<uint64_t>(GetCurrentThreadId()));
#elif defined(CHROMATIC_DARWIN)
  uint64_t tid;
  pthread_threadid_np(nullptr, &tid);
  return std::make_shared<NativePointer>(tid);
#else
  return std::make_shared<NativePointer>(static_cast<uint64_t>(
      reinterpret_cast<uintptr_t>(reinterpret_cast<void *>(pthread_self()))));
#endif
}

std::vector<std::shared_ptr<ModuleInfo>> NativeProcess::enumerateModules() {
  std::vector<std::shared_ptr<ModuleInfo>> result;

#ifdef CHROMATIC_WINDOWS
  HMODULE hMods[1024];
  DWORD cbNeeded;
  HANDLE hProcess = GetCurrentProcess();

  if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
    int count = cbNeeded / sizeof(HMODULE);
    for (int i = 0; i < count; i++) {
      MODULEINFO modInfo;
      char modName[MAX_PATH];

      if (GetModuleInformation(hProcess, hMods[i], &modInfo, sizeof(modInfo)) &&
          GetModuleFileNameExA(hProcess, hMods[i], modName, sizeof(modName))) {
        std::string fullPath = modName;
        std::string name = fullPath;
        auto pos = name.find_last_of("\\/");
        if (pos != std::string::npos)
          name = name.substr(pos + 1);

        result.push_back(std::make_shared<ModuleInfo>(ModuleInfo{
            name, std::make_shared<NativePointer>(reinterpret_cast<uint64_t>(modInfo.lpBaseOfDll)),
             static_cast<int>(modInfo.SizeOfImage), fullPath}));
      }
    }
  }

#elif defined(CHROMATIC_DARWIN)
  uint32_t count = _dyld_image_count();
  for (uint32_t i = 0; i < count; i++) {
    const char *imageName = _dyld_get_image_name(i);
    const struct mach_header *header = _dyld_get_image_header(i);

    if (!imageName || !header)
      continue;

    int imageSize = 0;
    std::vector<SegmentInfo> regions;
    if (header->magic == MH_MAGIC_64) {
      auto layout = describeMachoImage(
          reinterpret_cast<const struct mach_header_64 *>(header),
          _dyld_get_image_vmaddr_slide(i));
      imageSize = layout.size;
      regions = std::move(layout.regions);
    }

    std::string fullPath = imageName;
    std::string name = fullPath;
    auto pos = name.find_last_of('/');
    if (pos != std::string::npos)
      name = name.substr(pos + 1);

    result.push_back(std::make_shared<ModuleInfo>(ModuleInfo{
        name, std::make_shared<NativePointer>(reinterpret_cast<uint64_t>(header)),
                      imageSize, fullPath, std::move(regions)}));
  }

#elif defined(CHROMATIC_LINUX) || defined(CHROMATIC_ANDROID)
  // Parse /proc/self/maps to find loaded shared objects
  std::ifstream maps("/proc/self/maps");
  std::string line;

  struct LinuxModInfo {
    std::string name;
    std::string path;
    uint64_t base;
    uint64_t end;
    std::vector<SegmentInfo> segments;
  };
  std::vector<LinuxModInfo> modules;

  while (std::getline(maps, line)) {
    uint64_t start, end;
    char perms[5];
    uint64_t offset;
    unsigned int devMaj, devMin;
    unsigned long inode;
    char pathname[512] = {0};

    if (sscanf(line.c_str(), "%lx-%lx %4s %lx %x:%x %lu %511s", &start, &end,
               perms, &offset, &devMaj, &devMin, &inode, pathname) >= 7) {
      if (pathname[0] == '/' || pathname[0] == '[') {
        std::string pathStr = pathname;
        bool found = false;
        for (auto &m : modules) {
          if (m.path == pathStr) {
            if (start < m.base)
              m.base = start;
            if (end > m.end)
              m.end = end;
            m.segments.push_back({start, end - start});
            found = true;
            break;
          }
        }
        if (!found && pathname[0] == '/') {
          std::string name = pathStr;
          auto pos = name.find_last_of('/');
          if (pos != std::string::npos)
            name = name.substr(pos + 1);
          modules.push_back(
              {name, pathStr, start, end, {{start, end - start}}});
        }
      }
    }
  }

  for (const auto &m : modules) {
    result.push_back(std::make_shared<ModuleInfo>(ModuleInfo{
        m.name, std::make_shared<NativePointer>(m.base),
                      static_cast<int>(m.end - m.base), m.path, m.segments}));
  }
#endif

  return result;
}

std::vector<std::shared_ptr<RangeInfo>>
NativeProcess::enumerateRanges(const std::string &protection) {
  std::vector<std::shared_ptr<RangeInfo>> result;

  auto matchesProt = [&](const std::string &rangeProt) -> bool {
    if (protection.empty())
      return true;
    for (char c : protection) {
      if (c != '-' && rangeProt.find(c) == std::string::npos)
        return false;
    }
    return true;
  };

#ifdef CHROMATIC_WINDOWS
  MEMORY_BASIC_INFORMATION mbi;
  auto addr = reinterpret_cast<const uint8_t *>(0);

  while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
    if (mbi.State == MEM_COMMIT) {
      std::string prot;
      switch (mbi.Protect & 0xFF) {
      case PAGE_EXECUTE_READWRITE:
        prot = "rwx";
        break;
      case PAGE_EXECUTE_READ:
        prot = "r-x";
        break;
      case PAGE_READWRITE:
        prot = "rw-";
        break;
      case PAGE_READONLY:
        prot = "r--";
        break;
      case PAGE_EXECUTE:
        prot = "--x";
        break;
      default:
        prot = "---";
      }

      if (matchesProt(prot)) {
        result.push_back(std::make_shared<RangeInfo>(RangeInfo{
            std::make_shared<NativePointer>(reinterpret_cast<uint64_t>(mbi.BaseAddress)),
             static_cast<int>(mbi.RegionSize), prot, ""}));
      }
    }
    addr += mbi.RegionSize;
    if (reinterpret_cast<uintptr_t>(addr) == 0)
      break;
  }

#elif defined(CHROMATIC_LINUX) || defined(CHROMATIC_ANDROID)
  std::ifstream maps("/proc/self/maps");
  std::string line;

  while (std::getline(maps, line)) {
    uint64_t start, end;
    char perms[5];
    char pathname[512] = {0};

    if (sscanf(line.c_str(), "%lx-%lx %4s %*x %*x:%*x %*lu %511s", &start, &end,
               perms, pathname) >= 3) {
      std::string prot;
      prot += (perms[0] == 'r') ? 'r' : '-';
      prot += (perms[1] == 'w') ? 'w' : '-';
      prot += (perms[2] == 'x') ? 'x' : '-';

      if (matchesProt(prot)) {
        std::string filePath;
        if (pathname[0] == '/')
          filePath = pathname;
        result.push_back(std::make_shared<RangeInfo>(RangeInfo{
            std::make_shared<NativePointer>(start), static_cast<int>(end - start), prot, filePath}));
      }
    }
  }

#elif defined(CHROMATIC_DARWIN)
  mach_port_t task = mach_task_self();
  vm_address_t address = 0;
  vm_size_t vmsize = 0;

  while (true) {
    struct vm_region_basic_info_64 info;
    mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objectName;

    kern_return_t kr = vm_region_64(
        task, &address, &vmsize, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &infoCount, &objectName);

    if (kr != KERN_SUCCESS)
      break;

    std::string prot;
    prot += (info.protection & VM_PROT_READ) ? 'r' : '-';
    prot += (info.protection & VM_PROT_WRITE) ? 'w' : '-';
    prot += (info.protection & VM_PROT_EXECUTE) ? 'x' : '-';

    if (matchesProt(prot)) {
      result.push_back(std::make_shared<RangeInfo>(RangeInfo{
          std::make_shared<NativePointer>(address), static_cast<int>(vmsize), prot, ""}));
    }

    address += vmsize;
  }
#endif

  return result;
}

std::shared_ptr<NativePointer> NativeProcess::findExportByName(const std::string &moduleName,
                                            const std::string &exportName) {
#ifdef CHROMATIC_WINDOWS
  if (moduleName.empty()) {
    HMODULE hMods[1024];
    DWORD cbNeeded = 0;
    HANDLE hProcess = GetCurrentProcess();

    if (!EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded))
      return std::make_shared<NativePointer>(0);

    const auto count = static_cast<size_t>(cbNeeded / sizeof(HMODULE));
    for (size_t i = 0; i < count; i++) {
      if (FARPROC proc = GetProcAddress(hMods[i], exportName.c_str()))
        return std::make_shared<NativePointer>(reinterpret_cast<uint64_t>(proc));
    }
    return std::make_shared<NativePointer>(0);
  }

  HMODULE hMod = GetModuleHandleA(moduleName.c_str());
  if (!hMod)
    return std::make_shared<NativePointer>(0);
  if (FARPROC proc = GetProcAddress(hMod, exportName.c_str()))
    return std::make_shared<NativePointer>(reinterpret_cast<uint64_t>(proc));
  return std::make_shared<NativePointer>(0);
#else
  void *handle = nullptr;
  if (moduleName.empty()) {
    handle = RTLD_DEFAULT;
  } else {
    handle = dlopen(moduleName.c_str(), RTLD_NOLOAD | RTLD_LAZY);
    if (!handle) {
      handle = RTLD_DEFAULT;
    }
  }
  void *sym = dlsym(handle, exportName.c_str());
  if (!sym)
    return std::make_shared<NativePointer>(0);
  return std::make_shared<NativePointer>(reinterpret_cast<uint64_t>(sym));
#endif
}

std::shared_ptr<ModuleInfo>
NativeProcess::findModuleByAddress(std::shared_ptr<NativePointer> address) {
  uint64_t addr = address->value();

#ifdef CHROMATIC_WINDOWS
  HMODULE hMod;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(addr), &hMod))
    return nullptr;

  MODULEINFO modInfo;
  char modName[MAX_PATH];
  HANDLE hProcess = GetCurrentProcess();

  if (!GetModuleInformation(hProcess, hMod, &modInfo, sizeof(modInfo)) ||
      !GetModuleFileNameExA(hProcess, hMod, modName, sizeof(modName)))
    return nullptr;

  std::string fullPath = modName;
  std::string name = fullPath;
  auto pos = name.find_last_of("\\/");
  if (pos != std::string::npos)
    name = name.substr(pos + 1);

  return std::make_shared<ModuleInfo>(ModuleInfo{
      name, std::make_shared<NativePointer>(reinterpret_cast<uint64_t>(modInfo.lpBaseOfDll)),
                    static_cast<int>(modInfo.SizeOfImage), fullPath});

#elif defined(CHROMATIC_DARWIN)
  uint32_t count = _dyld_image_count();
  for (uint32_t i = 0; i < count; i++) {
    const struct mach_header *header = _dyld_get_image_header(i);
    if (!header)
      continue;

    uint64_t base = reinterpret_cast<uint64_t>(header);
    int imageSize = 0;
    std::vector<SegmentInfo> regions;

    if (header->magic == MH_MAGIC_64) {
      auto layout = describeMachoImage(
          reinterpret_cast<const struct mach_header_64 *>(header),
          _dyld_get_image_vmaddr_slide(i));
      imageSize = layout.size;
      regions = std::move(layout.regions);
    }

    // An image in the shared cache is not one contiguous range, so ask its
    // regions rather than assuming base + size covers it.
    const bool contains =
        !regions.empty()
            ? std::any_of(regions.begin(), regions.end(),
                          [addr](const SegmentInfo &region) {
                            return addr >= region.base &&
                                   addr < region.base + region.size;
                          })
            : (addr >= base && addr < base + static_cast<uint64_t>(imageSize));

    if (contains) {
      const char *imageName = _dyld_get_image_name(i);
      std::string fullPath = imageName ? imageName : "";
      std::string name = fullPath;
      auto pos = name.find_last_of('/');
      if (pos != std::string::npos)
        name = name.substr(pos + 1);

      return std::make_shared<ModuleInfo>(ModuleInfo{
          name, std::make_shared<NativePointer>(base), imageSize, fullPath,
          std::move(regions)});
    }
  }
  return nullptr;

#elif defined(CHROMATIC_LINUX) || defined(CHROMATIC_ANDROID)
  Dl_info info;
  if (dladdr(reinterpret_cast<void *>(addr), &info) && info.dli_fname) {
    std::string fullPath = info.dli_fname;
    std::string name = fullPath;
    auto pos = name.find_last_of('/');
    if (pos != std::string::npos)
      name = name.substr(pos + 1);

    uint64_t base = reinterpret_cast<uint64_t>(info.dli_fbase);
    return std::make_shared<ModuleInfo>(ModuleInfo{name, std::make_shared<NativePointer>(base), 0, fullPath});
  }
  return nullptr;
#else
  return nullptr;
#endif
}

std::shared_ptr<ModuleInfo>
NativeProcess::findModuleByName(const std::string &name) {
  auto modules = enumerateModules();
  for (const auto &m : modules) {
    if (m->name == name)
      return m;
  }
  return nullptr;
}

std::vector<std::shared_ptr<ExportInfo>>
NativeProcess::enumerateExports(const std::string &moduleName) {
  std::vector<std::shared_ptr<ExportInfo>> result;

#ifdef CHROMATIC_WINDOWS
  HMODULE hMod =
      GetModuleHandleA(moduleName.empty() ? nullptr : moduleName.c_str());
  if (!hMod)
    return result;

  auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
  if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    return result;

  auto ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
      reinterpret_cast<uint8_t *>(hMod) + dosHeader->e_lfanew);
  auto &exportDir =
      ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

  if (exportDir.VirtualAddress == 0)
    return result;

  auto exports = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(
      reinterpret_cast<uint8_t *>(hMod) + exportDir.VirtualAddress);
  auto names = reinterpret_cast<DWORD *>(reinterpret_cast<uint8_t *>(hMod) +
                                         exports->AddressOfNames);
  auto functions = reinterpret_cast<DWORD *>(reinterpret_cast<uint8_t *>(hMod) +
                                             exports->AddressOfFunctions);
  auto ordinals = reinterpret_cast<WORD *>(reinterpret_cast<uint8_t *>(hMod) +
                                           exports->AddressOfNameOrdinals);

  for (DWORD i = 0; i < exports->NumberOfNames; i++) {
    auto expName = reinterpret_cast<const char *>(
        reinterpret_cast<uint8_t *>(hMod) + names[i]);
    auto funcAddr = reinterpret_cast<uint64_t>(
        reinterpret_cast<uint8_t *>(hMod) + functions[ordinals[i]]);

    result.push_back(std::make_shared<ExportInfo>(ExportInfo{"function", expName, std::make_shared<NativePointer>(funcAddr)}));
  }

#elif defined(CHROMATIC_DARWIN)
  void *handle = dlopen(moduleName.c_str(), RTLD_NOLOAD | RTLD_LAZY);
  if (!handle && !moduleName.empty()) {
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
      const char *imgName = _dyld_get_image_name(i);
      if (imgName) {
        std::string path = imgName;
        std::string shortName = path;
        auto pos = shortName.find_last_of('/');
        if (pos != std::string::npos)
          shortName = shortName.substr(pos + 1);
        if (shortName == moduleName || path == moduleName) {
          const struct mach_header *header = _dyld_get_image_header(i);
          intptr_t slide = _dyld_get_image_vmaddr_slide(i);

          if (header && header->magic == MH_MAGIC_64) {
            auto header64 =
                reinterpret_cast<const struct mach_header_64 *>(header);
            auto cmd =
                reinterpret_cast<const struct load_command *>(header64 + 1);

            const struct symtab_command *symtab = nullptr;
            const struct segment_command_64 *linkedit = nullptr;
            const struct segment_command_64 *text = nullptr;

            for (uint32_t j = 0; j < header64->ncmds; j++) {
              if (cmd->cmd == LC_SYMTAB) {
                symtab = reinterpret_cast<const struct symtab_command *>(cmd);
              } else if (cmd->cmd == LC_SEGMENT_64) {
                auto seg =
                    reinterpret_cast<const struct segment_command_64 *>(cmd);
                if (strcmp(seg->segname, SEG_LINKEDIT) == 0)
                  linkedit = seg;
                else if (strcmp(seg->segname, SEG_TEXT) == 0)
                  text = seg;
              }
              cmd = reinterpret_cast<const struct load_command *>(
                  reinterpret_cast<const uint8_t *>(cmd) + cmd->cmdsize);
            }

            if (symtab && linkedit && text) {
              uint64_t fileOff =
                  linkedit->vmaddr - text->vmaddr - linkedit->fileoff;
              auto syms = reinterpret_cast<const struct nlist_64 *>(
                  reinterpret_cast<uintptr_t>(header) + symtab->symoff +
                  fileOff);
              auto strs = reinterpret_cast<const char *>(
                  reinterpret_cast<uintptr_t>(header) + symtab->stroff +
                  fileOff);

              for (uint32_t s = 0; s < symtab->nsyms; s++) {
                if ((syms[s].n_type & N_EXT) &&
                    (syms[s].n_type & N_TYPE) == N_SECT) {
                  const char *symName = strs + syms[s].n_un.n_strx;
                  if (symName[0] == '_')
                    symName++;
                  uint64_t symAddr = syms[s].n_value + slide;

                  result.push_back(std::make_shared<ExportInfo>(ExportInfo{"function", symName, std::make_shared<NativePointer>(symAddr)}));
                }
              }
            }
          }
          break;
        }
      }
    }
  }
  if (handle)
    dlclose(handle);

#elif defined(CHROMATIC_LINUX) || defined(CHROMATIC_ANDROID)
  // Walk loaded shared objects via dl_iterate_phdr to find the target module,
  // then parse its ELF dynamic symbol table (.dynsym / .dynstr).
  struct IterCtx {
    const std::string *moduleName;
    std::vector<std::shared_ptr<ExportInfo>> *result;
  };
  IterCtx ctx{&moduleName, &result};

  dl_iterate_phdr(
      [](struct dl_phdr_info *info, size_t, void *data) -> int {
        auto &ctx = *static_cast<IterCtx *>(data);
        std::string path = info->dlpi_name ? info->dlpi_name : "";
        std::string shortName = path;
        auto pos = shortName.find_last_of('/');
        if (pos != std::string::npos)
          shortName = shortName.substr(pos + 1);

        if (shortName != *ctx.moduleName && path != *ctx.moduleName)
          return 0; // continue iteration

        // Find PT_DYNAMIC
        const ElfW(Dyn) *dyn = nullptr;
        for (int i = 0; i < info->dlpi_phnum; i++) {
          if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const ElfW(Dyn) *>(
                info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
          }
        }
        if (!dyn)
          return 1; // stop

        const ElfW(Sym) *symtab = nullptr;
        const char *strtab = nullptr;
        size_t nchain = 0; // from DT_HASH
        const uint32_t *gnuBuckets = nullptr;
        size_t gnuNbuckets = 0;
        uint32_t gnuSymndx = 0; // from DT_GNU_HASH

        for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
          switch (d->d_tag) {
          case DT_SYMTAB:
            symtab = reinterpret_cast<const ElfW(Sym) *>(d->d_un.d_ptr);
            break;
          case DT_STRTAB:
            strtab = reinterpret_cast<const char *>(d->d_un.d_ptr);
            break;
          case DT_HASH: {
            auto hash = reinterpret_cast<const uint32_t *>(d->d_un.d_ptr);
            nchain = hash[1]; // nchain == number of symbols
            break;
          }
          case DT_GNU_HASH: {
            auto gh = reinterpret_cast<const uint32_t *>(d->d_un.d_ptr);
            gnuNbuckets = gh[0];
            gnuSymndx = gh[1];
            // bloom filter: gh[2] bloom-word count; each bloom word is
            // ElfW(Addr)-sized (8 bytes on 64-bit = 2 × uint32_t).
            // Buckets immediately follow the bloom filter.
            uint32_t bloomWords = gh[2];
            uint32_t bloomU32s = bloomWords * (sizeof(ElfW(Addr)) / sizeof(uint32_t));
            gnuBuckets = gh + 4 + bloomU32s;
            break;
          }
          }
        }

        if (!symtab || !strtab)
          return 1; // stop

        // Determine symbol count: prefer DT_HASH, fall back to DT_GNU_HASH
        size_t nsyms = nchain;
        if (nsyms == 0 && gnuBuckets) {
          // Find max bucket value, then walk chain from there
          uint32_t maxSym = 0;
          for (size_t i = 0; i < gnuNbuckets; i++) {
            if (gnuBuckets[i] > maxSym)
              maxSym = gnuBuckets[i];
          }
          if (maxSym >= gnuSymndx) {
            const uint32_t *chains = gnuBuckets + gnuNbuckets;
            uint32_t idx = maxSym - gnuSymndx;
            while (!(chains[idx] & 1))
              idx++;
            nsyms = gnuSymndx + idx + 1;
          }
        }

        for (size_t i = 0; i < nsyms; i++) {
          const auto &sym = symtab[i];
          if (sym.st_shndx == SHN_UNDEF || sym.st_value == 0)
            continue;
          unsigned char bind = ELF64_ST_BIND(sym.st_info);
          if (bind != STB_GLOBAL && bind != STB_WEAK)
            continue;
          const char *name = strtab + sym.st_name;
          if (!name[0])
            continue;
          unsigned char type = ELF64_ST_TYPE(sym.st_info);
          const char *typeStr = (type == STT_FUNC) ? "function" : "variable";
          uint64_t addr = info->dlpi_addr + sym.st_value;
          ctx.result->push_back(std::make_shared<ExportInfo>(ExportInfo{typeStr, name, std::make_shared<NativePointer>(addr)}));
        }
        return 1; // stop — found our module
      },
      &ctx);
#endif

  return result;
}

} // namespace chromatic::js
