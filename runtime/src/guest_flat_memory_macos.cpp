#include "guest_flat_memory.h"

#include <mach/mach.h>
#if defined(MKW_PLATFORM_VISIONOS)
// The iOS family's SDK refuses <mach/mach_vm.h>; the vm_map.h entry points
// <mach/mach.h> brings in are the same calls with vm_address_t (64-bit here).
#include <mach/vm_map.h>
#include <fcntl.h>
#else
#include <mach/mach_vm.h>
#endif
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "memory_access.h"
#include "runtime_log.h"

// Flat guest memory on Darwin (macOS and visionOS).
//
// macOS backs each region with anonymous memory and aliases it into the 4 GiB
// reservation with mach_vm_remap; with Apple Silicon's 16 KiB pages it keeps
// every access on the checked Memory::Read*/Write* path.
//
// visionOS (whose SDK has no mach_vm_remap) backs each region with an unlinked
// temporary file mapped twice, and runs the flat path: the translated code
// reads and writes guest memory through the flat view at kFixedFlatGuestBase
// with plain loads and stores. Three things the other backends catch with page
// protections and a fault handler are handled here too, on 16 KiB pages:
//   - deferred EFB reads: the destination of a pending copy is made PROT_NONE
//     (rounded out to host pages) and materialized when first touched;
//   - MMIO reads: the 32 MiB window is PROT_NONE, and a touch is reported
//     fatally, as elsewhere (writes there are caught inline, FlatWriteNeedsPolicy);
//   - unmapped guest addresses read as zero and swallow writes: the 4 GiB
//     reservation is demand-zero memory, so there is nothing to trap.
// The executable-write guard (a diagnostic for mods patching untranslated code)
// is not implemented here: a 16 KiB page holds code and data side by side.
// MKW_CHECKED_GUEST_MEMORY=1 in the environment restores the checked path
// there, which is several times slower for memory-heavy guest code.

namespace GuestFlat {
bool g_requiresCheckedAccess = false;
namespace {

struct Store {
    Backing kind = Backing::Owned;
    uint32_t owned = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

struct Mapping {
    uint32_t base = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

struct GuardedRange { uint32_t start; uint32_t end; };

std::mutex g_mutex;
std::vector<Store> g_stores;
std::vector<Mapping> g_mappings;
std::vector<RegionRequest> g_layout;
std::vector<GuardedRange> g_deferred; // under g_mutex
uint8_t* g_base = nullptr;
bool g_active = false;
uint64_t g_hostPage = 0x4000;
std::atomic<uint32_t> g_countEfb{0};
std::atomic<uint32_t> g_countMmio{0};

constexpr uint32_t kMmioStart = 0xCC000000u;
constexpr uint32_t kMmioSize = 0x02000000u;

inline uint64_t RoundUp(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

uint64_t PageDown(uint64_t address) { return address & ~(g_hostPage - 1); }
uint64_t PageUp(uint64_t address) { return (address + g_hostPage - 1) & ~(g_hostPage - 1); }
bool Overlaps(uint64_t aStart, uint64_t aEnd, uint64_t bStart, uint64_t bEnd) { return aStart < bEnd && bStart < aEnd; }
bool Protect(uint64_t first, uint64_t last, int protection) {
    return mprotect(g_base + first, static_cast<size_t>(last - first), protection) == 0;
}

uint64_t Offset(const RegionRequest& r) {
    if (r.backing == Backing::Mem1) return r.base & 0x1fffffffu;
    if (r.backing == Backing::Mem2) return (r.base & 0x1fffffffu) - 0x10000000u;
    return 0;
}

bool Same(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.base == y.base && x.size == y.size && x.backing == y.backing; });
}

#if defined(MKW_PLATFORM_VISIONOS)
// The store every alias of a region maps: an unlinked temporary file. The iOS family (visionOS)
// sandboxes /tmp away and names the app's own temporary directory in TMPDIR instead.
int BackingFile(size_t size) {
    std::string name;
    if (const char* tmp = std::getenv("TMPDIR"); tmp && *tmp) {
        name = tmp;
        if (name.back() != '/') name += '/';
    }
    if (name.empty()) name = "./";
    name += "wiicompiled-guest-XXXXXX";
    const int fd = mkstemp(name.data());
    if (fd >= 0) { unlink(name.c_str()); if (ftruncate(fd, static_cast<off_t>(size)) != 0) { close(fd); return -1; } }
    return fd;
}

// What sits in the way of the fixed reservation, and which other bases the
// process could have taken: the region the kernel reports at the fixed base
// and a probe of a few candidate bases (reserved and released at once). For
// the log when the reservation fails, so a change in the system's layout (as
// visionOS 27 brought) can be read off the crash instead of guessed at.
std::string ReserveDiagnostics(kern_return_t failure) {
    std::string out = " [vm_allocate=" + std::to_string(failure);
    vm_address_t probe = kFixedFlatGuestBase;
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &probe, &size, VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &count, &object) == KERN_SUCCESS) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "; region at/after base: 0x%llx+0x%llx prot=%d/%d shared=%d",
                      static_cast<unsigned long long>(probe), static_cast<unsigned long long>(size),
                      static_cast<int>(info.protection), static_cast<int>(info.max_protection),
                      static_cast<int>(info.shared));
        out += buffer;
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    } else {
        out += "; no region at or after the base";
    }
    vm_address_t anywhere = 0;
    if (vm_allocate(mach_task_self(), &anywhere, kGuestSpaceSize, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "; anywhere -> 0x%llx", static_cast<unsigned long long>(anywhere));
        out += buffer;
        vm_deallocate(mach_task_self(), anywhere, kGuestSpaceSize);
    }
    // Every region of the map, so the gaps show.
    out += "; map:";
    vm_address_t cursor = 0;
    for (int i = 0; i < 400; ++i) {
        vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t regionInfo{};
        mach_msg_type_number_t regionCount = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t regionObject = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &cursor, &regionSize, VM_REGION_BASIC_INFO_64,
                         reinterpret_cast<vm_region_info_t>(&regionInfo), &regionCount, &regionObject) != KERN_SUCCESS)
            break;
        if (regionObject != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), regionObject);
        // Only what matters for a 4 GiB reservation: regions from 256 MiB up.
        if (regionSize >= 0x1000'0000ull) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), " %llx+%llx/%d", static_cast<unsigned long long>(cursor),
                          static_cast<unsigned long long>(regionSize), static_cast<int>(regionInfo.protection));
            out += buffer;
        }
        cursor += regionSize;
    }
    out += "; free bases:";
    for (uint64_t candidate = 0x1'0000'0000ull; candidate <= 0x200'0000'0000ull; candidate += 0x4'0000'0000ull) {
        vm_address_t address = candidate;
        if (vm_allocate(mach_task_self(), &address, kGuestSpaceSize, VM_FLAGS_FIXED) == KERN_SUCCESS &&
            address == candidate) {
            vm_deallocate(mach_task_self(), address, kGuestSpaceSize);
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), " %lluG", static_cast<unsigned long long>(candidate >> 30));
            out += buffer;
        }
    }
    return out + "]";
}

std::string ReserveFailureMessage(kern_return_t failure) {
    return "unable to reserve the fixed 4 GiB guest address space at " +
           std::to_string(kFixedFlatGuestBase >> 30) + " GiB; the visionOS app needs the "
           "com.apple.developer.kernel.extended-virtual-addressing entitlement (see visionos/), and the "
           "system's layout may have moved (guest_abi/flat_memory.h)" + ReserveDiagnostics(failure);
}

// Two aliases of one unlinked file per store: the host view and the guest view.
void MapFileBackedRegions(const std::vector<RegionRequest>& regions, uint64_t pageSize) {
    struct FileStore { Backing kind; uint32_t owned; uint64_t size; int fd; };
    std::vector<FileStore> stores;
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto it = std::find_if(stores.begin(), stores.end(), [&](const FileStore& s) { return s.kind == r.backing && s.owned == owned; });
        const uint64_t need = RoundUp(Offset(r) + r.size, pageSize);
        if (it == stores.end()) stores.push_back({r.backing, owned, need, -1}); else it->size = std::max(it->size, need);
    }
    for (auto& s : stores) { s.fd = BackingFile(s.size); if (s.fd < 0) throw std::runtime_error("unable to create visionOS guest backing store"); }
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        const auto& s = *std::find_if(stores.begin(), stores.end(), [&](const FileStore& x) { return x.kind == r.backing && x.owned == owned; });
        auto* host = static_cast<uint8_t*>(mmap(nullptr, r.size, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, Offset(r)));
        auto* guest = mmap(g_base + r.base, r.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s.fd, Offset(r));
        if (host == MAP_FAILED || guest != g_base + r.base) throw std::runtime_error("unable to map visionOS guest alias");
        g_mappings.push_back({r.base, r.size, host});
    }
    for (auto& s : stores) close(s.fd);
}
#endif

} // namespace

bool IsActive() { return g_active; }

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard lock(g_mutex);
    const uint64_t pageSize = static_cast<uint64_t>(getpagesize());
    g_hostPage = pageSize;
#if defined(MKW_PLATFORM_VISIONOS)
    // The flat path is the default whatever the page size (see the notes at the top).
    const char* checked = std::getenv("MKW_CHECKED_GUEST_MEMORY");
    g_requiresCheckedAccess = checked != nullptr && checked[0] == '1';
#else
    g_requiresCheckedAccess = pageSize > kGuestPageSize;
#endif
    if (g_active) {
        if (!Same(g_layout, regions))
            throw std::runtime_error("flat guest layout cannot be remapped");
        return;
    }

    for (size_t i = 0; i < regions.size(); ++i) {
        const auto& r = regions[i];
        if (r.size == 0) continue;
        if (r.size > kGuestSpaceSize - r.base ||
            r.base % pageSize != 0 || Offset(r) % pageSize != 0) {
            throw std::runtime_error("invalid flat guest memory region");
        }
        const uint64_t end = static_cast<uint64_t>(r.base) + RoundUp(r.size, pageSize);
        for (size_t j = 0; j < i; ++j) {
            const auto& prior = regions[j];
            if (prior.size == 0) continue;
            const uint64_t priorEnd = static_cast<uint64_t>(prior.base) + RoundUp(prior.size, pageSize);
            if (r.base < priorEnd && prior.base < end) {
                throw std::runtime_error("overlapping flat guest memory regions");
            }
        }
    }

#if defined(MKW_PLATFORM_VISIONOS)
    vm_address_t address = kFixedFlatGuestBase;
    if (const kern_return_t result = vm_allocate(mach_task_self(), &address, kGuestSpaceSize, VM_FLAGS_FIXED);
        result != KERN_SUCCESS || address != kFixedFlatGuestBase)
        throw std::runtime_error(ReserveFailureMessage(result));
    g_base = reinterpret_cast<uint8_t*>(address);
    MapFileBackedRegions(regions, pageSize);
    g_layout = regions;
    g_active = true;
#else
    mach_vm_address_t address = kFixedFlatGuestBase;
    if (mach_vm_allocate(mach_task_self(), &address, kGuestSpaceSize, VM_FLAGS_FIXED) != KERN_SUCCESS || address != kFixedFlatGuestBase) {
        throw std::runtime_error("unable to reserve fixed 4 GiB macOS guest address space");
    }
    g_base = reinterpret_cast<uint8_t*>(address);

    std::vector<Store> stores;
    try {
        for (const auto& r : regions) {
            if (!r.size) continue;
            const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
            auto it = std::find_if(stores.begin(), stores.end(), [&](const Store& s) {
                return s.kind == r.backing && s.owned == owned;
            });
            const uint64_t need = RoundUp(Offset(r) + r.size, pageSize);
            if (it == stores.end()) {
                stores.push_back({r.backing, owned, need, nullptr});
            } else {
                it->size = std::max(it->size, need);
            }
        }

        for (auto& s : stores) {
            void* host = mmap(nullptr, static_cast<size_t>(s.size), PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
            if (host == MAP_FAILED) {
                throw std::runtime_error("unable to allocate anonymous macOS guest backing store");
            }
            s.host = static_cast<uint8_t*>(host);
        }

        for (const auto& r : regions) {
            if (!r.size) continue;
            const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
            const auto& s = *std::find_if(stores.begin(), stores.end(), [&](const Store& x) {
                return x.kind == r.backing && x.owned == owned;
            });
            mach_vm_address_t target = reinterpret_cast<mach_vm_address_t>(g_base + r.base);
            vm_prot_t cur = VM_PROT_NONE, max = VM_PROT_NONE;
            const kern_return_t kr = mach_vm_remap(
                mach_task_self(),
                &target,
                static_cast<mach_vm_size_t>(RoundUp(r.size, pageSize)),
                0,
                VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                mach_task_self(),
                reinterpret_cast<mach_vm_address_t>(s.host + Offset(r)),
                FALSE,
                &cur,
                &max,
                VM_INHERIT_NONE
            );
            if (kr != KERN_SUCCESS) {
                throw std::runtime_error(std::string("mach_vm_remap guest alias failed: ") + mach_error_string(kr));
            }
            g_mappings.push_back({r.base, r.size, s.host + Offset(r)});
        }

        g_layout = regions;
        g_stores = std::move(stores);
        g_active = true;
    } catch (...) {
        for (auto& s : stores) {
            if (s.host != nullptr) {
                munmap(s.host, static_cast<size_t>(s.size));
            }
        }
        mach_vm_deallocate(mach_task_self(), address, kGuestSpaceSize);
        g_mappings.clear();
        g_stores.clear();
        g_layout.clear();
        g_base = nullptr;
        throw;
    }
#endif

    if (!g_requiresCheckedAccess) {
        // Hardware registers have no backing store: a read there is a missing HLE hook,
        // reported from the fault handler rather than answered with a zero that would hang
        // the caller in a status poll.
        Protect(kMmioStart, static_cast<uint64_t>(kMmioStart) + kMmioSize, PROT_NONE);
    }
    RT_LOG(RT_TAG_MEMORY) << "flat guest memory: " << (g_requiresCheckedAccess ? "checked" : "flat")
                          << " access, host page " << (g_hostPage >> 10) << " KiB" << std::endl;
}

uint8_t* HostPointer(uint32_t a) {
    for (const auto& m : g_mappings) {
        if (a >= m.base && uint64_t(a - m.base) < m.size) {
            return m.host + (a - m.base);
        }
    }
    return nullptr;
}

void ProtectDeferredRange(uint32_t address, size_t length) {
    if (RequiresCheckedAccess() || !g_active || length == 0) return;
    const uint64_t end = static_cast<uint64_t>(address) + length;
    if (end > kGuestSpaceSize) return;
    std::lock_guard lock(g_mutex);
    // Rounded out to host pages: the neighbours in the same page fault too, and the
    // handler materializes every deferred read of the page, so nothing stale is served.
    if (!Protect(PageDown(address), PageUp(end), PROT_NONE)) return;
    g_deferred.push_back(GuardedRange{address, static_cast<uint32_t>(end)});
}

void UnprotectDeferredRange(uint32_t address, size_t length) {
    if (RequiresCheckedAccess() || !g_active || length == 0) return;
    std::lock_guard lock(g_mutex);
    const uint64_t end = static_cast<uint64_t>(address) + length;
    const auto it = std::find_if(g_deferred.begin(), g_deferred.end(), [&](const GuardedRange& range) {
        return range.start == address && range.end == static_cast<uint32_t>(end);
    });
    if (it == g_deferred.end()) return;
    g_deferred.erase(it);
    // The pages open again unless another pending range still shares one of them.
    const uint64_t first = PageDown(address);
    const uint64_t last = PageUp(end);
    const bool shared = std::any_of(g_deferred.begin(), g_deferred.end(), [&](const GuardedRange& range) {
        return Overlaps(first, last, PageDown(range.start), PageUp(range.end));
    });
    if (!shared) Protect(first, last, PROT_READ | PROT_WRITE);
}

void RegisterExecutableRange(uint32_t, uint32_t) {}

FaultCounters Counters() {
    FaultCounters counters;
    counters.efb = g_countEfb.load(std::memory_order_relaxed);
    counters.mmio = g_countMmio.load(std::memory_order_relaxed);
    return counters;
}

void LogFaultSummary() noexcept {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_relaxed)) return;
    const FaultCounters counters = Counters();
    RT_LOG(RT_TAG_MEMORY) << "shutdown summary: efb=" << counters.efb << " mmio=" << counters.mmio << std::endl;
}

bool HandleAccessViolation(void* faultAddress, bool isWrite) noexcept {
    if (!g_active || g_base == nullptr || faultAddress == nullptr) return false;
    const uintptr_t fault = reinterpret_cast<uintptr_t>(faultAddress);
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
    if (fault < base || fault - base >= kGuestSpaceSize) return false;
    const uint32_t guestAddress = static_cast<uint32_t>(fault - base);

    // A deferred (EFB) read. The page is opened, with every pending range that shares
    // it (and the pages those spill into, and so on), then all of it is materialized.
    uint64_t first = PageDown(guestAddress);
    uint64_t last = first + g_hostPage;
    bool covered = false;
    {
        std::lock_guard lock(g_mutex);
        for (bool grew = true; grew;) {
            grew = false;
            for (auto it = g_deferred.begin(); it != g_deferred.end();) {
                const uint64_t rangeFirst = PageDown(it->start);
                const uint64_t rangeLast = PageUp(it->end);
                if (!Overlaps(first, last, rangeFirst, rangeLast)) { ++it; continue; }
                covered = true;
                if (rangeFirst < first) { first = rangeFirst; grew = true; }
                if (rangeLast > last) { last = rangeLast; grew = true; }
                it = g_deferred.erase(it);
            }
        }
        if (covered) Protect(first, last, PROT_READ | PROT_WRITE);
    }
    if (covered) {
        g_countEfb.fetch_add(1, std::memory_order_relaxed);
        try {
            MemoryInline::ResolveDeferredReads(static_cast<uint32_t>(first), static_cast<size_t>(last - first));
        } catch (const std::exception& error) {
            RT_LOG(RT_TAG_MEMORY) << "FATAL deferred read materialization failed at 0x" << std::hex << guestAddress
                                  << std::dec << ": " << error.what() << std::endl;
            std::abort();
        }
        return true;
    }

    if (!RequiresCheckedAccess() && guestAddress >= kMmioStart && guestAddress - kMmioStart < kMmioSize) {
        g_countMmio.fetch_add(1, std::memory_order_relaxed);
        RT_LOG(RT_TAG_MEMORY) << "FATAL MMIO " << (isWrite ? "write" : "read") << " reached the flat memory path at 0x"
                              << std::hex << std::uppercase << guestAddress << std::dec
                              << " (hardware registers have no backing store; add HLE for this device)" << std::endl;
        std::abort();
    }
    return false;
}

} // namespace GuestFlat
