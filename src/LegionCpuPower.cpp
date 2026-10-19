// <wtl> project: legioncpupower | file: src/LegionCpuPower.cpp | author: watchthelight </wtl>
//
// LegionCpuPower: lets the Core Ultra 9 275HX (Arrow Lake-HX) in the Legion 5 15IAX10 run at its real clocks and power
// under macOS 26.
//
// Two independent fixes, both on at boot (boot-args -legioncpunohwp / -legioncpunopl turn one off) and switchable at
// runtime (sudo sysctl -w debug.legioncpu.<name>=0|1):
//
// hwp  XCPM writes IA32_HWP_REQUEST (0x774) with min/max in 100 MHz ratios (max 54 = 5.4 GHz from MSR 0x1AD). On Arrow
//      Lake the HWP fields are in per-core-type performance units (about 61 MHz on P-cores, 70 MHz on E-cores here), so
//      max 54 stops P-cores at 3.3 GHz and E-cores at 3.8 GHz. With hwp=1 this kext clears XCPM's _xcpm_hwp_enabled flag
//      (XCPM then writes IA32_PERF_CTL, which the CPU ignores while HWP is enabled) and programs each core's request
//      itself: min = lowest, max = highest from IA32_HWP_CAPABILITIES (0x771), desired 0 (autonomous), EPP from
//      debug.legioncpu.epp. The request is checked every second and written again if anything changed it.
//
// pl   The firmware keeps a second package power limit in MCHBAR + 0x59A0 and sets it to about 30 W at boot; Lenovo's
//      Windows software raises it, nothing does under macOS. The CPU obeys the lower of it and MSR 0x610 (PL1 120 W,
//      PL2 168 W, locked). With pl=1 this kext copies 0x610's PL1/PL2 fields into MCHBAR (unless MCHBAR is locked) and
//      keeps them there; with pl=0 it puts the firmware's value back. If the package stays at 100 C or more for 30 s it
//      puts the firmware value back until the package has been below 85 C for a minute. Thermal MSRs and TjMax are never
//      touched.
//
// Gates: CPUID family 6 model 0xC5/0xC6 (read with the instruction, not the OpenCore-spoofed kernel copy), Tahoe only,
// and the _xcpm_hwp_enabled symbol must resolve. Boot-arg -legioncpuoff disables the kext.

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/sysctl.h>

extern "C" {
int cpu_number(void);
void mp_rendezvous_no_intrs(void (*action)(void *), void *arg);
int rdmsr_carefully(uint32_t msr, uint32_t *lo, uint32_t *hi);
}

namespace {

constexpr uint32_t kMsrPkgPowerLimit = 0x610, kMsrPkgThermStatus = 0x1B1, kMsrTempTarget = 0x1A2;
constexpr uint32_t kMsrHwpCaps = 0x771, kMsrHwpRequest = 0x774;
constexpr unsigned kMaxCpus = 64;

volatile uint32_t *gXcpmHwpEnabled;
bool gCpuOk;
int gHwp = 1, gPl = 1, gEpp = 0x20, gHotSeconds, gCoolSeconds, gPlHeld;
uint32_t gHolds;
char gStatus[256] = "starting";
thread_call_t gTick;
IOMemoryMap *gMchMap;
volatile uint32_t *gMchPl;  // MCHBAR + 0x59A0
uint64_t gMchOriginal;
bool gMchSaved;
int gHwpApplied, gPlApplied;
uint32_t gRewrites;

inline void wrmsr64(uint32_t msr, uint64_t v) {
    asm volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
inline uint64_t rdmsr64(uint32_t msr, bool *ok = nullptr) {
    uint32_t lo = 0, hi = 0;
    bool r = rdmsr_carefully(msr, &lo, &hi) == 0;
    if (ok) *ok = r;
    return ((uint64_t)hi << 32) | lo;
}

bool arrowLake() {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    uint32_t family = (a >> 8) & 0xF, model = ((a >> 4) & 0xF) | (((a >> 16) & 0xF) << 4);
    return family == 6 && (model == 0xC5 || model == 0xC6);
}

struct HwpPass { int epp; uint32_t changed; };

// runs on every CPU with interrupts off
void hwpPerCpu(void *arg) {
    auto *p = static_cast<HwpPass *>(arg);
    bool ok = false;
    uint64_t caps = rdmsr64(kMsrHwpCaps, &ok);
    if (!ok) return;
    uint64_t highest = caps & 0xFF, lowest = (caps >> 24) & 0xFF;
    uint64_t want = lowest | (highest << 8) | ((uint64_t)(p->epp & 0xFF) << 24);  // desired 0 = autonomous
    uint64_t cur = rdmsr64(kMsrHwpRequest, &ok);
    if (ok && (cur & 0xFFFFFFFFULL) != want) {
        wrmsr64(kMsrHwpRequest, (cur & ~0xFFFFFFFFULL & ~(1ULL << 42)) | want);
        __atomic_fetch_add(&p->changed, 1, __ATOMIC_RELAXED);
    }
}

bool mapMchbar() {
    if (gMchPl) return true;
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    if (!match) return false;
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (!it) return false;
    IOPCIDevice *host = nullptr;
    while (OSObject *o = it->getNextObject()) {
        auto *d = OSDynamicCast(IOPCIDevice, o);
        if (d && d->getBusNumber() == 0 && d->getDeviceNumber() == 0 && d->getFunctionNumber() == 0 &&
            (d->configRead32(0) & 0xFFFF) == 0x8086) {
            host = d;
            break;
        }
    }
    if (!host) { it->release(); return false; }
    uint64_t bar = ((uint64_t)host->configRead32(0x4C) << 32) | host->configRead32(0x48);
    it->release();
    if (!(bar & 1)) return false;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((bar & 0x7FFFFFE0000ULL) + 0x5000, 0x1000,
                                                                     kIODirectionInOut);
    if (!md) return false;
    gMchMap = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
    md->release();
    if (!gMchMap) return false;
    gMchPl = reinterpret_cast<volatile uint32_t *>(gMchMap->getVirtualAddress() + 0x9A0);
    gMchOriginal = ((uint64_t)gMchPl[1] << 32) | gMchPl[0];
    gMchSaved = true;
    SYSLOG("lcp", "MCHBAR power limit at boot 0x%llx", (unsigned long long)gMchOriginal);
    return true;
}

void writeMch(uint64_t v) {
    gMchPl[0] = (uint32_t)v;
    gMchPl[1] = (uint32_t)(v >> 32);
}

int pkgTempC() {
    bool ok1 = false, ok2 = false;
    uint64_t tt = rdmsr64(kMsrTempTarget, &ok1), ts = rdmsr64(kMsrPkgThermStatus, &ok2);
    if (!ok1 || !ok2) return -1;
    return (int)((tt >> 16) & 0xFF) - (int)((ts >> 16) & 0x7F);
}

void tick(thread_call_param_t, thread_call_param_t) {
    // hwp
    if (gHwp && gXcpmHwpEnabled) {
        *gXcpmHwpEnabled = 0;
        HwpPass p{gEpp, 0};
        mp_rendezvous_no_intrs(hwpPerCpu, &p);
        gRewrites += p.changed;
        gHwpApplied = 1;
    } else if (!gHwp && gHwpApplied && gXcpmHwpEnabled) {
        *gXcpmHwpEnabled = 1;  // XCPM takes the request back at its next p-state change
        gHwpApplied = 0;
    }
    // pl, with a heat guard: 30 s at 100 C or more puts the firmware limit back ("held"); a minute below 85 C releases it
    int temp = pkgTempC();
    if (gPl && gPlHeld) {
        gCoolSeconds = (temp >= 0 && temp < 85) ? gCoolSeconds + 1 : 0;
        if (gCoolSeconds >= 60) {
            SYSLOG("lcp", "package back to %d C: power limit raised again", temp);
            gPlHeld = 0;
            gCoolSeconds = 0;
        }
    }
    if (gPl && !gPlHeld && mapMchbar()) {
        if (gMchOriginal >> 63) {
            snprintf(gStatus, sizeof(gStatus), "MCHBAR power limit locked (0x%llx); pl not applied",
                     (unsigned long long)gMchOriginal);
            gPl = 0;
        } else {
            uint64_t want = rdmsr64(kMsrPkgPowerLimit) & ~(1ULL << 63);
            uint64_t cur = ((uint64_t)gMchPl[1] << 32) | gMchPl[0];
            if (cur != want) writeMch(want);
            gPlApplied = 1;
            gHotSeconds = temp >= 100 ? gHotSeconds + 1 : 0;
            if (gHotSeconds >= 30) {
                SYSLOG("lcp", "package at %d C for 30 s: firmware power limit until it cools below 85 C", temp);
                gPlHeld = 1;
                gHotSeconds = 0;
                gHolds++;
            }
        }
    }
    if ((!gPl || gPlHeld) && gPlApplied && gMchSaved) {
        writeMch(gMchOriginal);
        gPlApplied = 0;
    }
    if (strncmp(gStatus, "MCHBAR power limit locked", 25) != 0)
        snprintf(gStatus, sizeof(gStatus),
                 "cpu %s, xcpm hwp flag %s, hwp %d (rewrites %u), pl %d%s (heat holds %u), epp %d, pkg %d C, mchbar %s",
                 gCpuOk ? "arrow lake" : "unsupported", gXcpmHwpEnabled ? (*gXcpmHwpEnabled ? "1" : "0") : "missing",
                 gHwpApplied, gRewrites, gPlApplied, gPlHeld ? " held for heat" : "", gHolds, gEpp, temp,
                 gMchPl ? "mapped" : "not mapped");
    uint64_t deadline;
    clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(gTick, deadline);
}

int sysctlInt(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req) {
    int v = *static_cast<int *>(arg1);
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (arg1 == &gEpp) {
        if (v < 0 || v > 255) return EINVAL;
    } else if (v != 0 && v != 1) {
        return EINVAL;
    }
    if (arg1 == &gHwp && v && !gXcpmHwpEnabled) return ENOTSUP;
    if (!gCpuOk && v && arg1 != &gEpp) return ENOTSUP;
    *static_cast<int *>(arg1) = v;
    return 0;
}

}  // namespace
extern struct sysctl_oid sysctl__debug_legioncpu, sysctl__debug_legioncpu_hwp, sysctl__debug_legioncpu_pl,
    sysctl__debug_legioncpu_epp, sysctl__debug_legioncpu_status;
namespace {
void pluginStart() {
    gCpuOk = arrowLake();
    if (!gCpuOk) gHwp = gPl = 0;
    if (checkKernelArgument("-legioncpunohwp")) gHwp = 0;
    if (checkKernelArgument("-legioncpunopl")) gPl = 0;
    SYSLOG("lcp", "start, cpu %s", gCpuOk ? "Arrow Lake" : "not Arrow Lake: switches stay off");
    sysctl_register_oid(&sysctl__debug_legioncpu);
    sysctl_register_oid(&sysctl__debug_legioncpu_hwp);
    sysctl_register_oid(&sysctl__debug_legioncpu_pl);
    sysctl_register_oid(&sysctl__debug_legioncpu_epp);
    sysctl_register_oid(&sysctl__debug_legioncpu_status);
    lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
        if (gCpuOk) {
            auto addr = patcher.solveSymbol(KernelPatcher::KernelID, "_xcpm_hwp_enabled");
            if (addr) gXcpmHwpEnabled = reinterpret_cast<volatile uint32_t *>(addr);
            else { SYSLOG("lcp", "_xcpm_hwp_enabled not found: hwp stays off"); gHwp = 0; }
            patcher.clearError();
        }
        gTick = thread_call_allocate(tick, nullptr);
        uint64_t deadline;
        clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
        if (gTick) thread_call_enter_delayed(gTick, deadline);
    });
}

}  // namespace

SYSCTL_NODE(_debug, OID_AUTO, legioncpu, CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, "LegionCpuPower");
SYSCTL_PROC(_debug_legioncpu, OID_AUTO, hwp, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &gHwp, 0,
            sysctlInt, "I", "1 = program HWP requests per core (highest/lowest), 0 = leave them to XCPM");
SYSCTL_PROC(_debug_legioncpu, OID_AUTO, pl, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &gPl, 0,
            sysctlInt, "I", "1 = MCHBAR power limit follows MSR 0x610, 0 = firmware value");
SYSCTL_PROC(_debug_legioncpu, OID_AUTO, epp, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &gEpp, 0,
            sysctlInt, "I", "EPP written with hwp=1 (0 performance .. 255 power saving)");
SYSCTL_STRING(_debug_legioncpu, OID_AUTO, status, CTLFLAG_RD | CTLFLAG_LOCKED, gStatus, sizeof(gStatus), "state");


static const char *bootargOff[] {"-legioncpuoff"};
static const char *bootargDebug[] {"-legioncpudbg"};
static const char *bootargBeta[] {"-legioncpubeta"};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Tahoe,
    KernelVersion::Tahoe,
    pluginStart
};
