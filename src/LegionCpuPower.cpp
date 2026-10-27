// <wtl> project: legioncpupower | file: src/LegionCpuPower.cpp | author: watchthelight </wtl>
//
// LegionCpuPower: lets the Core Ultra 9 275HX (Arrow Lake-HX) in the Legion 5 15IAX10 run at its real clocks and power
// under macOS 26.
//
// Three switches, all on at boot (boot-args -legioncpunohwp / -legioncpunopl / -legioncpunofan turn one off) and switchable at
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
//      keeps them there; with pl=0 it puts the firmware's value back. Heat guard (debug.legioncpu.guard, default 1):
//      a minute at 100 C or more caps PL1/PL2 at the measured package power minus 10 W (never below 45 W), and again
//      after every further minute that hot; each 30 s below 90 C raises the cap 5 W until it reaches 0x610's limits.
//      Thermal MSRs and TjMax are never touched, so the CPU's own throttling at TjMax stays in charge.
//
// fan  0 auto (the EC's own curve), 1 full speed, 2 custom curve (debug.legioncpu.fancurve), 3 low (the EC curve at
//      60 % for the lower eight points, the two hottest kept). Full speed is Lenovo's WMI feature 0x04020000 through
//      \_SB.GZFD.WMAE SetFeatureValue (sets EC bit FNST, EC RAM 0xFE50058B bit 0, under the firmware's EC mutex), the
//      call Lenovo Vantage makes on Windows. Curves go through \_SB.GZFD.WMAB method 6 (EC F9F0-F9F9) and are read
//      back with method 5; the curve found at boot is restored when leaving modes 2 and 3. Checked every 5 s. In modes 2
//      and 3, 10 s at 95 C or more switches to full speed until the package has been below 80 C for a minute.
//      On this EC generation (0x5508) the EC only follows FNST and the table in Lenovo's Custom power mode, so modes
//      1-3 switch to Custom (GZFD.WMAA 0x2C with 0xFF; AC only) and fan=0 puts the previous mode back. Curve values are
//      fan levels 1-10 (LenovoLegionLinux model_s2cn). No mode change in the first two minutes after boot (GPU bring-up).
//
// sched macOS does not know P-cores from E-cores. On Intel the scheduler picks an idle CPU in choose_processor_smt
//      (xnu osfmk/kern/sched_prim.c): first from pset->perfcontrol_cpu_preferred_bitmask, then from CPUs outside
//      perfcontrol_cpu_migration_bitmask, then any idle CPU, and it moves a waking thread off its last CPU when that
//      CPU is in the migration mask and a CPU outside it is idle. Only Apple Silicon's Edge scheduler writes the two
//      masks, so on Intel they stay 0. With sched=1 this kext sets preferred = the favoured P-cores (highest HWP
//      performance) and migration = the E-cores, so work goes to the fastest cores first, then the other P-cores, and
//      to E-cores only when no P-core is idle. Under full load every CPU is still used. The pset field offsets
//      (+0x8a0, +0x8a8, 25G241) are checked against the instructions in choose_processor_smt before any write.
//
// Gates: CPUID family 6 model 0xC5/0xC6 (read with the instruction, not the OpenCore-spoofed kernel copy), Tahoe only,
// and the _xcpm_hwp_enabled symbol must resolve. Boot-arg -legioncpuoff disables the kext.

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
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

constexpr uint32_t kMsrRaplUnit = 0x606, kMsrPkgEnergy = 0x611, kMsrPkgPowerLimit = 0x610, kMsrPkgThermStatus = 0x1B1, kMsrTempTarget = 0x1A2;
constexpr uint32_t kMsrHwpCaps = 0x771, kMsrHwpRequest = 0x774;
constexpr unsigned kMaxCpus = 64;

volatile uint32_t *gXcpmHwpEnabled;
bool gCpuOk;
int gHwp = 1, gPl = 1, gGuard = 1, gFan = 0, gEpp = 0x20, gHotSeconds, gCoolSeconds, gCapW, gPkgW = -1;
uint32_t gHolds, gLastEnergy;
uint64_t gLastEnergyTime;
char gStatus[512] = "starting";
thread_call_t gTick;
IOMemoryMap *gMchMap;
volatile uint32_t *gMchPl;  // MCHBAR + 0x59A0
uint64_t gMchOriginal;
bool gMchSaved;
int gHwpApplied, gPlApplied;
uint32_t gRewrites;
IOACPIPlatformDevice *gGzfd;
int gFanApplied, gFanRpm1, gFanRpm2, gFanState = -1, gFanHot, gFanCool, gFanRescue, gCurveTouched;
unsigned gTicks;
int gSched = 1, gSchedApplied;
uint64_t gMaskFav, gMaskP, gMaskE;
volatile uint64_t *gPsetPreferred, *gPsetMigration;
const char *gSchedWhy = "starting";
const uint8_t *gChooseProc;
void **gPsetArray;
uint8_t gCurveNow[10], gCurveBoot[10], gCurveWant[10];
bool gCurveNowOk, gCurveBootOk, gCurveWantSet;
int gPowerMode = -1, gPrevMode = -1;  // WMAA 0x2D: 1 quiet, 2 balanced, 3 performance, 0xFF custom, 0xE0 extreme
const uint8_t kLowCurve[10] = {1, 1, 2, 2, 3, 4, 5, 6, 8, 10};

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
volatile uint8_t gHighest[64];  // HWP highest per cpu number, filled by hwpPerCpu (or highestPerCpu with hwp=0)

void highestPerCpu(void *) {
    bool ok = false;
    uint64_t caps = rdmsr64(kMsrHwpCaps, &ok);
    int cpu = cpu_number();
    if (ok && cpu >= 0 && cpu < 64) gHighest[cpu] = (uint8_t)(caps & 0xFF);
}

// true if the 0x400 bytes after p contain "8b <modrm> <disp32>" for this displacement (any register)
bool readsDisp(const uint8_t *p, uint32_t disp) {
    for (int i = 0; i < 0x400 - 6; i++)
        if (p[i] == 0x8b && (p[i + 1] & 0xC0) == 0x80 &&
            (p[i + 2] | p[i + 3] << 8 | p[i + 4] << 16 | (uint32_t)p[i + 5] << 24) == disp)
            return true;
    return false;
}

// core types from HWP highest, then the pset fields, checked before use; false = try again next tick
bool schedSetup() {
    if (!gChooseProc || !gPsetArray) { gSchedWhy = "scheduler symbols missing"; return true; }
    auto *pset = static_cast<uint8_t *>(gPsetArray[0]);
    if (!pset) { gSchedWhy = "pset_array[0] empty"; return true; }
    uint64_t recommended = *reinterpret_cast<volatile uint64_t *>(pset + 0x30), seen = 0;
    uint8_t top = 0;
    for (int c = 0; c < 64; c++) {
        if (gHighest[c]) seen |= 1ULL << c;
        top = gHighest[c] > top ? gHighest[c] : top;
    }
    if (!recommended || (seen & recommended) != recommended) { gSchedWhy = "waiting for all cpus"; return false; }
    gMaskFav = gMaskP = gMaskE = 0;
    for (int c = 0; c < 64; c++) {
        if (!(recommended >> c & 1)) continue;
        if (gHighest[c] >= 80) gMaskP |= 1ULL << c; else gMaskE |= 1ULL << c;
        if (gHighest[c] == top) gMaskFav |= 1ULL << c;
    }
    if (!readsDisp(gChooseProc, 0x8a0) || !readsDisp(gChooseProc, 0x8a8)) {
        gSchedWhy = "choose_processor_smt does not match 25G241 (pset +0x8a0/+0x8a8)";
        return true;
    }
    auto *pref = reinterpret_cast<volatile uint64_t *>(pset + 0x8a0), *mig = reinterpret_cast<volatile uint64_t *>(pset + 0x8a8);
    if (*pref || *mig || (recommended & (gMaskP | gMaskE)) != (gMaskP | gMaskE) || !gMaskE || gMaskFav == (gMaskP | gMaskE)) {
        gSchedWhy = "pset fields do not look as expected";
        SYSLOG("lcp", "sched off: preferred 0x%llx migration 0x%llx recommended 0x%llx P 0x%llx E 0x%llx",
               (unsigned long long)*pref, (unsigned long long)*mig, (unsigned long long)recommended,
               (unsigned long long)gMaskP, (unsigned long long)gMaskE);
        return true;
    }
    gPsetPreferred = pref;
    gPsetMigration = mig;
    gSchedWhy = "ok";
    SYSLOG("lcp", "sched: favoured 0x%llx P 0x%llx E 0x%llx", (unsigned long long)gMaskFav, (unsigned long long)gMaskP,
           (unsigned long long)gMaskE);
    return true;
}
bool gSchedSetupDone;

// runs on every CPU with interrupts off
void hwpPerCpu(void *arg) {
    auto *p = static_cast<HwpPass *>(arg);
    bool ok = false;
    uint64_t caps = rdmsr64(kMsrHwpCaps, &ok);
    if (!ok) return;
    uint64_t highest = caps & 0xFF, lowest = (caps >> 24) & 0xFF;
    int cpu = cpu_number();
    if (cpu >= 0 && cpu < 64) gHighest[cpu] = (uint8_t)highest;
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

// \_SB.GZFD.<name>(0, method, buffer); the caller releases *result
bool gzfd(const char *name, uint32_t method, const void *buf, unsigned len, OSObject **result) {
    if (!gGzfd) {
        gGzfd = OSDynamicCast(IOACPIPlatformDevice, IORegistryEntry::fromPath("IOACPIPlane:/_SB/GZFD"));
        if (!gGzfd) return false;
    }
    OSObject *params[3] = {OSNumber::withNumber(0ULL, 32), OSNumber::withNumber(method, 32), OSData::withBytes(buf, len)};
    IOReturn r = (params[0] && params[1] && params[2]) ? gGzfd->evaluateObject(name, result, params, 3)
                                                       : kIOReturnNoMemory;
    for (auto *o : params) OSSafeReleaseNULL(o);
    return r == kIOReturnSuccess;
}

// WMAE 0x11 GetFeatureValue, 0x12 SetFeatureValue
bool wmae(uint32_t method, uint32_t feature, uint32_t value, uint32_t *out) {
    uint32_t args[2] = {feature, value};
    OSObject *result = nullptr;
    bool ok = gzfd("WMAE", method, args, 8, &result);
    if (ok && out) {
        auto *n = OSDynamicCast(OSNumber, result);
        if (n) *out = n->unsigned32BitValue(); else ok = false;
    }
    OSSafeReleaseNULL(result);
    return ok;
}

// WMAB 5: count dword, then F9F0-F9F9 as dwords
bool readCurve(uint8_t out[10]) {
    uint32_t zero = 0;
    OSObject *result = nullptr;
    bool ok = gzfd("WMAB", 5, &zero, 4, &result);
    auto *d = OSDynamicCast(OSData, result);
    ok = ok && d && d->getLength() >= 44;
    if (ok) {
        auto *w = static_cast<const uint32_t *>(d->getBytesNoCopy());
        for (int i = 0; i < 10; i++) out[i] = (uint8_t)w[1 + i];
    }
    OSSafeReleaseNULL(result);
    return ok;
}

// WMAB 6: points as bytes at 6, 8, .., 0x18
void writeCurve(const uint8_t v[10]) {
    uint8_t buf[0x1A] = {};
    for (int i = 0; i < 10; i++) buf[6 + 2 * i] = v[i];
    OSObject *result = nullptr;
    gzfd("WMAB", 6, buf, sizeof(buf), &result);
    OSSafeReleaseNULL(result);
}

// GZFD.WMAA: 0x2C set power mode, 0x2D get
bool wmaa(uint32_t method, uint32_t arg, uint32_t *out) {
    OSObject *result = nullptr;
    bool ok = gzfd("WMAA", method, &arg, 4, &result);
    if (ok && out) {
        auto *n = OSDynamicCast(OSNumber, result);
        if (n) *out = n->unsigned32BitValue(); else ok = false;
    }
    OSSafeReleaseNULL(result);
    return ok;
}

void fan(int temp) {
    uint32_t v = 0;
    if (wmaa(0x2D, 0, &v)) gPowerMode = (int)v;
    // a mode change notifies NPCF and the GPU; during the NVIDIA driver's display bring-up it kept the framebuffer
    // heads from publishing, so no mode change in the first two minutes after boot
    uint64_t now, limit;
    clock_get_uptime(&now);
    nanoseconds_to_absolutetime(120ULL * NSEC_PER_SEC, &limit);
    if (now < limit) {
    } else if (gFan || gFanRescue) {
        if (gPowerMode >= 0 && gPowerMode != 0xFF) {
            if (gPrevMode < 0) gPrevMode = gPowerMode;
            wmaa(0x2C, 0xFF, nullptr);
            if (wmaa(0x2D, 0, &v)) gPowerMode = (int)v;
        }
    } else if (gPrevMode >= 0) {
        if (gPowerMode == 0xFF) wmaa(0x2C, (uint32_t)gPrevMode, nullptr);
        if (wmaa(0x2D, 0, &v)) gPowerMode = (int)v;
        gPrevMode = -1;
    }
    if (!wmae(0x11, 0x04020000, 0, &v)) { gFanState = -1; return; }
    gFanState = (int)v;
    gCurveNowOk = readCurve(gCurveNow);
    if (gCurveNowOk && !gCurveBootOk) {
        memcpy(gCurveBoot, gCurveNow, 10);
        gCurveBootOk = true;
        SYSLOG("lcp", "fan curve at boot %u,%u,%u,%u,%u,%u,%u,%u,%u,%u", gCurveBoot[0], gCurveBoot[1], gCurveBoot[2],
               gCurveBoot[3], gCurveBoot[4], gCurveBoot[5], gCurveBoot[6], gCurveBoot[7], gCurveBoot[8], gCurveBoot[9]);
    }
    // a quieter curve gives way to full speed when hot
    if (gFan >= 2) {
        gFanHot = temp >= 95 ? gFanHot + 1 : 0;
        if (gFanHot >= 2 && !gFanRescue) { gFanRescue = 1; SYSLOG("lcp", "package %d C: fans to full speed", temp); }
        if (gFanRescue) {
            gFanCool = (temp >= 0 && temp < 80) ? gFanCool + 1 : 0;
            if (gFanCool >= 12) { gFanRescue = 0; gFanCool = 0; }
        }
    } else {
        gFanRescue = gFanHot = gFanCool = 0;
    }
    bool full = gFan == 1 || gFanRescue;
    if (full) {
        if (v != 1) wmae(0x12, 0x04020000, 1, nullptr);
        gFanApplied = 1;
    } else if (gFanApplied) {
        if (v != 0) wmae(0x12, 0x04020000, 0, nullptr);
        gFanApplied = 0;
    }
    uint8_t want[10];
    bool custom = false;
    if (gFan == 2 && gCurveWantSet) {
        memcpy(want, gCurveWant, 10);
        custom = true;
    } else if (gFan == 3 && gCurveBootOk) {
        bool empty = true;
        for (int i = 0; i < 10; i++) empty = empty && !gCurveBoot[i];
        for (int i = 0; i < 10; i++)
            want[i] = empty ? kLowCurve[i] : (i >= 8 ? gCurveBoot[i] : (uint8_t)max(1, gCurveBoot[i] * 6 / 10));
        custom = true;
    } else if (gCurveTouched && gCurveBootOk) {
        memcpy(want, gCurveBoot, 10);
    } else {
        memcpy(want, gCurveNow, 10);
    }
    if (gCurveNowOk && memcmp(want, gCurveNow, 10) != 0) {
        writeCurve(want);
        memcpy(gCurveNow, want, 10);
    }
    gCurveTouched = custom;
    if (wmae(0x11, 0x04030001, 0, &v)) gFanRpm1 = (int)v;
    if (wmae(0x11, 0x04030002, 0, &v)) gFanRpm2 = (int)v;
}

void tick(thread_call_param_t, thread_call_param_t) {
    if (!gSchedSetupDone && gCpuOk && gTicks >= 2) {
        if (!gHwp) mp_rendezvous_no_intrs(highestPerCpu, nullptr);
        gSchedSetupDone = schedSetup();
    }
    if (gTicks++ % 5 == 0) fan(pkgTempC());
    // sched
    if (gPsetPreferred) {
        if (gSched) {
            if (*gPsetPreferred != gMaskFav) *gPsetPreferred = gMaskFav;
            if (*gPsetMigration != gMaskE) *gPsetMigration = gMaskE;
            gSchedApplied = 1;
        } else if (gSchedApplied) {
            *gPsetPreferred = 0;
            *gPsetMigration = 0;
            gSchedApplied = 0;
        }
    }
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
    // package power over the last tick, from the energy counter
    int temp = pkgTempC();
    uint64_t unit = rdmsr64(kMsrRaplUnit), nowAbs, ns;
    uint32_t energy = (uint32_t)rdmsr64(kMsrPkgEnergy);
    clock_get_uptime(&nowAbs);
    if (gLastEnergyTime) {
        absolutetime_to_nanoseconds(nowAbs - gLastEnergyTime, &ns);
        uint64_t uj = ((uint64_t)(energy - gLastEnergy) * 1000000ULL) >> ((unit >> 8) & 0x1F);
        if (ns) gPkgW = (int)(uj * 1000 / ns);
    }
    gLastEnergy = energy;
    gLastEnergyTime = nowAbs;
    // heat guard: step the cap down after each minute at 100 C or more, back up 5 W per 30 s below 90 C
    if (!gPl || !gGuard) {
        gCapW = gHotSeconds = gCoolSeconds = 0;
    } else {
        gHotSeconds = temp >= 100 ? gHotSeconds + 1 : 0;
        if (gHotSeconds >= 60) {
            int from = gCapW ? gCapW : gPkgW;
            gCapW = max(45, from - 10);
            gHotSeconds = 0;
            gHolds++;
            SYSLOG("lcp", "package %d C for a minute at %d W: power limit capped at %d W", temp, gPkgW, gCapW);
        }
        if (gCapW) {
            gCoolSeconds = (temp >= 0 && temp < 90) ? gCoolSeconds + 1 : 0;
            if (gCoolSeconds >= 30) {
                gCapW += 5;
                gCoolSeconds = 0;
                if (gCapW >= (int)(((rdmsr64(kMsrPkgPowerLimit) & 0x7FFF)) >> (unit & 0xF))) gCapW = 0;
            }
        }
    }
    if (gPl && mapMchbar()) {
        if (gMchOriginal >> 63) {
            snprintf(gStatus, sizeof(gStatus), "MCHBAR power limit locked (0x%llx); pl not applied",
                     (unsigned long long)gMchOriginal);
            gPl = 0;
        } else {
            uint64_t want = rdmsr64(kMsrPkgPowerLimit) & ~(1ULL << 63);
            if (gCapW) {
                uint64_t cap = (uint64_t)gCapW << (unit & 0xF);
                if ((want & 0x7FFF) > cap) want = (want & ~0x7FFFULL) | cap;
                if (((want >> 32) & 0x7FFF) > cap) want = (want & ~(0x7FFFULL << 32)) | (cap << 32);
            }
            uint64_t cur = ((uint64_t)gMchPl[1] << 32) | gMchPl[0];
            if (cur != want) writeMch(want);
            gPlApplied = 1;
        }
    }
    if (!gPl && gPlApplied && gMchSaved) {
        writeMch(gMchOriginal);
        gPlApplied = 0;
    }
    if (strncmp(gStatus, "MCHBAR power limit locked", 25) != 0)
        snprintf(gStatus, sizeof(gStatus),
                 "cpu %s, xcpm hwp flag %s, hwp %d (rewrites %u), pl %d (guard %d, cap %d W, steps %u), epp %d, pkg %d C %d W, "
                 "mchbar %s, "
                 "fan mode %d%s, power mode 0x%x%s, full speed %s (%d / %d rpm), sched %d (%s, favoured 0x%llx, avoid 0x%llx)",
                 gCpuOk ? "arrow lake" : "unsupported", gXcpmHwpEnabled ? (*gXcpmHwpEnabled ? "1" : "0") : "missing",
                 gHwpApplied, gRewrites, gPlApplied, gGuard, gCapW, gHolds, gEpp, temp, gPkgW,
                 gMchPl ? "mapped" : "not mapped", gFan, gFanRescue ? " (hot: full)" : "", gPowerMode,
                 (gFan && gPowerMode >= 0 && gPowerMode != 0xFF) ? " (custom needs AC)" : "",
                 gFanState < 0 ? "unknown" : (gFanState ? "on" : "off"), gFanRpm1,
                 gFanRpm2, gSchedApplied, gSchedWhy, (unsigned long long)gMaskFav, (unsigned long long)gMaskE);
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
    } else if (arg1 == &gFan) {
        if (v < 0 || v > 3) return EINVAL;
    } else if (v != 0 && v != 1) {
        return EINVAL;
    }
    if (arg1 == &gHwp && v && !gXcpmHwpEnabled) return ENOTSUP;
    if (!gCpuOk && v && arg1 != &gEpp && arg1 != &gFan) return ENOTSUP;
    *static_cast<int *>(arg1) = v;
    return 0;
}

// "a,b,..,j": ten points; reading shows the EC's current curve
int sysctlCurve(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
    char buf[64];
    const uint8_t *c = gCurveNowOk ? gCurveNow : gCurveWant;
    snprintf(buf, sizeof(buf), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9]);
    int err = sysctl_handle_string(oidp, buf, sizeof(buf), req);
    if (err || !req->newptr) return err;
    uint8_t v[10];
    const char *p = buf;
    for (int i = 0; i < 10; i++) {
        unsigned n = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) { n = n * 10 + (unsigned)(*p++ - '0'); digits++; }
        if (!digits || n > 255) return EINVAL;
        v[i] = (uint8_t)n;
        if (i < 9 && *p++ != ',') return EINVAL;
    }
    if (*p && *p != '\n') return EINVAL;
    memcpy(gCurveWant, v, 10);
    gCurveWantSet = true;
    return 0;
}

}  // namespace
extern struct sysctl_oid sysctl__debug_legioncpu, sysctl__debug_legioncpu_hwp, sysctl__debug_legioncpu_pl,
    sysctl__debug_legioncpu_sched, sysctl__debug_legioncpu_guard, sysctl__debug_legioncpu_fan, sysctl__debug_legioncpu_fancurve, sysctl__debug_legioncpu_epp, sysctl__debug_legioncpu_status;
namespace {
void pluginStart() {
    gCpuOk = arrowLake();
    if (!gCpuOk) gHwp = gPl = 0;
    if (checkKernelArgument("-legioncpunohwp")) gHwp = 0;
    if (checkKernelArgument("-legioncpunopl")) gPl = 0;
    if (checkKernelArgument("-legioncpunofan")) gFan = 0;
    if (checkKernelArgument("-legioncpunosched") || !gCpuOk) gSched = 0;
    SYSLOG("lcp", "start, cpu %s", gCpuOk ? "Arrow Lake" : "not Arrow Lake: switches stay off");
    sysctl_register_oid(&sysctl__debug_legioncpu);
    sysctl_register_oid(&sysctl__debug_legioncpu_hwp);
    sysctl_register_oid(&sysctl__debug_legioncpu_pl);
    sysctl_register_oid(&sysctl__debug_legioncpu_sched);
    sysctl_register_oid(&sysctl__debug_legioncpu_guard);
    sysctl_register_oid(&sysctl__debug_legioncpu_fan);
    sysctl_register_oid(&sysctl__debug_legioncpu_fancurve);
    sysctl_register_oid(&sysctl__debug_legioncpu_epp);
    sysctl_register_oid(&sysctl__debug_legioncpu_status);
    lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
        if (gCpuOk) {
            auto addr = patcher.solveSymbol(KernelPatcher::KernelID, "_xcpm_hwp_enabled");
            if (addr) gXcpmHwpEnabled = reinterpret_cast<volatile uint32_t *>(addr);
            else { SYSLOG("lcp", "_xcpm_hwp_enabled not found: hwp stays off"); gHwp = 0; }
            patcher.clearError();
            gChooseProc = reinterpret_cast<const uint8_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_choose_processor_smt"));
            patcher.clearError();
            gPsetArray = reinterpret_cast<void **>(patcher.solveSymbol(KernelPatcher::KernelID, "_pset_array"));
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
SYSCTL_PROC(_debug_legioncpu, OID_AUTO, sched, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &gSched, 0,
            sysctlInt, "I", "1 = scheduler prefers the favoured P-cores and avoids E-cores, 0 = stock");
SYSCTL_PROC(_debug_legioncpu, OID_AUTO, guard, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &gGuard, 0,
            sysctlInt, "I", "1 = heat guard steps the power limit down at 100 C, 0 = only the CPU's own throttling");
SYSCTL_PROC(_debug_legioncpu, OID_AUTO, fan, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, &gFan, 0,
            sysctlInt, "I", "fans: 0 auto (EC curve), 1 full speed, 2 custom curve, 3 low");
SYSCTL_PROC(_debug_legioncpu, OID_AUTO, fancurve, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0,
            sysctlCurve, "A", "custom fan curve for fan=2: ten comma-separated EC values");
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
