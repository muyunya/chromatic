#include "native_memory_access_monitor.h"
#include "native_pointer.h"
#include "native_exception_handler.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifdef CHROMATIC_WINDOWS
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef CHROMATIC_DARWIN
#include <mach/mach.h>
#include <mach/vm_region.h>
#endif

namespace {

uint64_t parseHexAddr(const std::string &s) {
  return std::stoull(s, nullptr, 16);
}

std::string toHexAddr(uint64_t addr) {
  std::ostringstream oss;
  oss << "0x" << std::hex << addr;
  return oss.str();
}

// ─── Watched range ───

struct WatchedRange {
  uint64_t pageBase; // page-aligned start
  size_t pageSize;   // page-aligned size
  int originalProt;  // saved original protection
  int rangeIndex;    // index in the user's ranges array
  bool fired;        // one-shot: true after first access
};

// ─── Pending access event (recorded in signal handler, drained in JS context)
// ───

struct PendingEvent {
  uint64_t monitorId;
  uint64_t faultAddress;
  uint64_t pageBase;
  std::string accessType;
  int rangeIndex;
};

// ─── Monitor entry ───

struct MonitorEntry {
  uint64_t id;
  std::vector<WatchedRange> ranges;
  std::function<void(std::shared_ptr<chromatic::js::NativePointer>, std::shared_ptr<chromatic::js::NativePointer>, std::string, int)> onAccess;
  chromatic::js::internal::ExceptionCallbackId segvHandlerId = 0;
  chromatic::js::internal::ExceptionCallbackId busHandlerId = 0;
};

static std::mutex g_monMutex;
static uint64_t g_nextMonId = 1;
static std::unordered_map<uint64_t, MonitorEntry *> g_monitors;

// ─── Pending events ring buffer (signal-safe write, JS-context drain) ───
// Fixed-size ring buffer to avoid malloc in signal handler.

static constexpr size_t MAX_PENDING = 256;

struct PendingRing {
  struct Entry {
    std::atomic<bool> ready{false};
    uint64_t monitorId;
    uint64_t faultAddress;
    uint64_t pageBase;
    int rangeIndex;
    // accessType stored as enum to avoid std::string in signal handler
    int accessTypeCode; // 0=unknown, 1=read, 2=write, 3=execute
  };
  Entry entries[MAX_PENDING];
  std::atomic<uint32_t> writeIdx{0};
  std::atomic<uint32_t> readIdx{0};
};

static PendingRing g_pending;

static const char *accessCodeToString(int code) {
  switch (code) {
  case 1:
    return "read";
  case 2:
    return "write";
  case 3:
    return "execute";
  default:
    return "unknown";
  }
}

// ─── Page size helper ───

static size_t getPageSize() {
#ifdef CHROMATIC_WINDOWS
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize;
#else
  return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

// ─── Protection helpers ───

/// Reads the protection currently on the page containing `addr`.
///
/// Needed because the monitor has to put the page back the way it found it. Assuming
/// read-write is wrong in both directions that matter: a read-only page comes back
/// writable, and a page of code comes back non-executable, so the instruction the
/// faulting thread is about to re-execute faults again - this time with no handler
/// interested - and the process dies.
///
/// Runs at enable() time, never in the signal handler, so reading a file is fine.
#ifdef CHROMATIC_WINDOWS
// VirtualProtect reports the previous protection in its out parameter; nothing to do.
#elif defined(CHROMATIC_DARWIN)
static bool queryProtection(uint64_t addr, int &prot) {
  vm_address_t regionAddr = static_cast<vm_address_t>(addr);
  vm_size_t regionSize = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  kern_return_t kr =
      vm_region_64(mach_task_self(), &regionAddr, &regionSize,
                   VM_REGION_BASIC_INFO_64,
                   reinterpret_cast<vm_region_info_t>(&info), &infoCount,
                   &object);
  if (kr != KERN_SUCCESS)
    return false;
  prot = static_cast<int>(info.protection) &
         (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
  return true;
}
#else
static bool queryProtection(uint64_t addr, int &prot) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps)
    return false;
  char line[512];
  bool found = false;
  while (fgets(line, sizeof(line), maps)) {
    unsigned long start = 0, end = 0;
    char perms[8] = {0};
    if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
      continue;
    if (addr >= start && addr < end) {
      prot = 0;
      if (perms[0] == 'r')
        prot |= PROT_READ;
      if (perms[1] == 'w')
        prot |= PROT_WRITE;
      if (perms[2] == 'x')
        prot |= PROT_EXEC;
      found = true;
      break;
    }
  }
  fclose(maps);
  return found;
}
#endif

static void setProtection(uint64_t addr, size_t size, int prot) {
#ifdef CHROMATIC_WINDOWS
  DWORD oldProt;
  VirtualProtect(reinterpret_cast<void *>(addr), size, static_cast<DWORD>(prot),
                 &oldProt);
#else
  mprotect(reinterpret_cast<void *>(addr), size, prot);
#endif
}

// ─── SIGSEGV handler (one-shot per range) ───
// This runs in signal context — no malloc, no JS, no mutex.
// Just restore permissions and record the event.

chromatic::js::HandleAction
monitorSegvHandler(std::shared_ptr<chromatic::js::ExceptionContext> ctx) {
  uint64_t faultAddr = ctx->faultAddress;

  // The monitor table is an unordered_map and every monitor holds a vector of ranges,
  // so walking them is only safe while no other thread is adding or removing a
  // monitor - and a fault can land on any thread while another is in enable() or
  // disable(). Both of those hold g_monMutex, so take it; with try_lock, because a
  // signal handler must never block. If it is unavailable, the fault is simply left
  // unhandled for whoever else registered for it.
  std::unique_lock<std::mutex> lock(g_monMutex, std::try_to_lock);
  if (!lock.owns_lock())
    return chromatic::js::HandleAction::NotHandled;

  for (auto &[id, monitor] : g_monitors) {
    for (auto &range : monitor->ranges) {
      if (range.fired)
        continue;
      if (faultAddr >= range.pageBase &&
          faultAddr < range.pageBase + range.pageSize) {
        // One-shot: mark as fired and restore permissions permanently
        range.fired = true;
        setProtection(range.pageBase, range.pageSize, range.originalProt);

        // Determine access type
        int accessCode = 0; // unknown
        switch (ctx->accessType) {
        case chromatic::js::AccessType::Read:
          accessCode = 1;
          break;
        case chromatic::js::AccessType::Write:
          accessCode = 2;
          break;
        case chromatic::js::AccessType::Execute:
          accessCode = 3;
          break;
        default:
          accessCode = 0;
          break;
        }

        // Record event in ring buffer (lock-free)
        uint32_t idx =
            g_pending.writeIdx.fetch_add(1, std::memory_order_relaxed) %
            MAX_PENDING;
        auto &entry = g_pending.entries[idx];
        entry.monitorId = monitor->id;
        entry.faultAddress = faultAddr;
        entry.pageBase = range.pageBase;
        entry.rangeIndex = range.rangeIndex;
        entry.accessTypeCode = accessCode;
        entry.ready.store(true, std::memory_order_release);

        // Return Handled — the faulting instruction will re-execute
        // with permissions restored.
        return chromatic::js::HandleAction::Handled;
      }
    }
  }

  return chromatic::js::HandleAction::NotHandled;
}

} // anonymous namespace

namespace chromatic::js {

std::string NativeMemoryAccessMonitor::enable(
    const std::vector<std::shared_ptr<NativePointer>> &addresses, const std::vector<int> &sizes,
    std::function<void(std::shared_ptr<NativePointer>, std::shared_ptr<NativePointer>, std::string, int)> onAccess) {

  if (addresses.size() != sizes.size())
    throw std::runtime_error(
        "addresses and sizes arrays must have the same length");

  std::lock_guard<std::mutex> lock(g_monMutex);

  internal::refEnable();

  auto *monitor = new MonitorEntry();
  monitor->id = g_nextMonId++;
  monitor->onAccess = std::move(onAccess);

  size_t pageSize = getPageSize();

  // Set up watched ranges
  for (size_t i = 0; i < addresses.size(); i++) {
    uint64_t addr = addresses[i]->value();
    size_t sz = static_cast<size_t>(sizes[i]);

    // Page-align
    uint64_t pageBase = addr & ~(pageSize - 1);
    size_t totalSize =
        ((addr + sz) - pageBase + pageSize - 1) & ~(pageSize - 1);

    WatchedRange range{};
    range.pageBase = pageBase;
    range.pageSize = totalSize;
    range.rangeIndex = static_cast<int>(i);
    range.fired = false;

#ifdef CHROMATIC_WINDOWS
    range.originalProt = PAGE_READWRITE; // replaced below by the real value
#else
    range.originalProt = PROT_READ | PROT_WRITE; // fallback if the query fails
#endif

    monitor->ranges.push_back(range);

    // Remove all permissions from the page
#ifdef CHROMATIC_WINDOWS
    DWORD oldProt;
    VirtualProtect(reinterpret_cast<void *>(pageBase), totalSize, PAGE_NOACCESS,
                   &oldProt);
    monitor->ranges.back().originalProt = static_cast<int>(oldProt);
#else
    int originalProt = PROT_READ | PROT_WRITE;
    queryProtection(pageBase, originalProt);
    monitor->ranges.back().originalProt = originalProt;
    mprotect(reinterpret_cast<void *>(pageBase), totalSize, PROT_NONE);
#endif
  }

  // Register exception handlers for SIGSEGV/AccessViolation and SIGBUS/BusError
  // On macOS ARM64, PROT_NONE writes raise SIGBUS, not SIGSEGV
  monitor->segvHandlerId = internal::registerHandler(
      ExceptionType::AccessViolation, monitorSegvHandler);
  monitor->busHandlerId =
      internal::registerHandler(ExceptionType::BusError, monitorSegvHandler);

  g_monitors[monitor->id] = monitor;
  return toHexAddr(monitor->id);
}

void NativeMemoryAccessMonitor::disable(const std::string &monitorIdStr) {
  // First drain any pending events for this monitor
  drainPending();

  uint64_t monId = parseHexAddr(monitorIdStr);

  std::lock_guard<std::mutex> lock(g_monMutex);

  auto it = g_monitors.find(monId);
  if (it == g_monitors.end())
    return;

  auto *monitor = it->second;

  // Restore original permissions for any ranges that haven't fired
  for (auto &range : monitor->ranges) {
    if (!range.fired) {
      setProtection(range.pageBase, range.pageSize, range.originalProt);
    }
  }

  // Unregister handlers
  internal::unregisterHandler(monitor->segvHandlerId);
  internal::unregisterHandler(monitor->busHandlerId);
  internal::refDisable();

  g_monitors.erase(it);
  delete monitor;
}

void NativeMemoryAccessMonitor::disableAll() {
  // Drain first
  drainPending();

  std::lock_guard<std::mutex> lock(g_monMutex);

  for (auto &[id, monitor] : g_monitors) {
    for (auto &range : monitor->ranges) {
      if (!range.fired) {
        setProtection(range.pageBase, range.pageSize, range.originalProt);
      }
    }
    internal::unregisterHandler(monitor->segvHandlerId);
    internal::unregisterHandler(monitor->busHandlerId);
    internal::refDisable();
    delete monitor;
  }
  g_monitors.clear();
}

int NativeMemoryAccessMonitor::drainPending() {
  int count = 0;
  uint32_t readIdx = g_pending.readIdx.load(std::memory_order_relaxed);
  uint32_t writeIdx = g_pending.writeIdx.load(std::memory_order_acquire);

  while (readIdx != writeIdx) {
    uint32_t idx = readIdx % MAX_PENDING;
    auto &entry = g_pending.entries[idx];

    // Wait until the entry is ready (the signal handler finished writing it)
    if (!entry.ready.load(std::memory_order_acquire))
      break;

    // Find the monitor and call its callback
    {
      std::lock_guard<std::mutex> lock(g_monMutex);
      auto it = g_monitors.find(entry.monitorId);
      if (it != g_monitors.end() && it->second->onAccess) {
        try {
          it->second->onAccess(
              std::make_shared<NativePointer>(entry.faultAddress), std::make_shared<NativePointer>(entry.pageBase),
              accessCodeToString(entry.accessTypeCode), entry.rangeIndex);
        } catch (...) {
          // Swallow
        }
      }
    }

    entry.ready.store(false, std::memory_order_release);
    readIdx++;
    g_pending.readIdx.store(readIdx, std::memory_order_release);
    count++;
  }

  return count;
}

} // namespace chromatic::js
