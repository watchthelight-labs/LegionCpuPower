// <wtl> project: legioncpupower | file: probe/LegionCpuProbe.cpp | author: watchthelight </wtl>
// Read-only CPU power diagnostic. Under macOS the 275HX tops out near 3.7 GHz on one thread and 3.05 GHz on all
// 24 at ~30 W, while XCPM allows ratio 54. This reads the limits below XCPM and publishes them as registry
// properties every 2 s (ioreg -r -c LegionCpuProbe): package power limits (MSR 0x610 and the MCHBAR mirror the
// firmware can set lower), turbo ratio limits, HWP capabilities/requests per CPU, perf status and the
// "limit reasons" bits that say why the cores are not boosting. Every MSR is read with rdmsr_carefully, so a
// register this CPU does not have shows up as missing instead of a #GP. Nothing is written.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <i386/proc_reg.h>

extern "C" {
int cpu_number(void);
void mp_rendezvous_no_intrs(void (*action)(void *), void *arg);
}

namespace {
constexpr unsigned kMaxCpus = 64;
// per logical CPU
constexpr uint32_t kPerCpuMsrs[] = {
    0x198,  // IA32_PERF_STATUS (current ratio)
    0x199,  // IA32_PERF_CTL
    0x19C,  // IA32_THERM_STATUS
    0x1B0,  // IA32_ENERGY_PERF_BIAS
    0x770,  // IA32_PM_ENABLE (HWP on)
    0x771,  // IA32_HWP_CAPABILITIES (highest/guaranteed/efficient/lowest)
    0x774,  // IA32_HWP_REQUEST (min/max/desired/EPP)
    0x64F,  // MSR_CORE_PERF_LIMIT_REASONS
};
constexpr unsigned kPerCpuCount = sizeof(kPerCpuMsrs) / sizeof(kPerCpuMsrs[0]);
// package scope (read once per tick on whatever CPU runs the timer)
constexpr uint32_t kPkgMsrs[] = {
    0xCE,   // MSR_PLATFORM_INFO (base/min ratios)
    0x1A2,  // MSR_TEMPERATURE_TARGET (TjMax, offset)
    0x1AD,  // MSR_TURBO_RATIO_LIMIT
    0x1AE,  // MSR_TURBO_RATIO_LIMIT1 (core counts / E-core groups)
    0x1B1,  // IA32_PACKAGE_THERM_STATUS
    0x601,  // MSR_VR_CURRENT_CONFIG (IccMax)
    0x606,  // MSR_RAPL_POWER_UNIT
    0x610,  // MSR_PKG_POWER_LIMIT (PL1/PL2, lock bit 63)
    0x611,  // MSR_PKG_ENERGY_STATUS
    0x614,  // MSR_PKG_POWER_INFO (TDP)
    0x638,  // MSR_PP0_POWER_LIMIT
    0x65C,  // MSR_PLATFORM_POWER_LIMIT (PSys)
    0x6B0,  // MSR_GRAPHICS_PERF_LIMIT_REASONS
    0x6B1,  // MSR_RING_PERF_LIMIT_REASONS
    0x772,  // IA32_HWP_REQUEST_PKG
};

struct CpuSample {
    uint64_t val[kMaxCpus][kPerCpuCount];
    uint32_t okMask[kMaxCpus];   // bit i = MSR i read fine
    uint32_t seen[kMaxCpus];
};

void readPerCpu(void *arg) {
    auto *s = static_cast<CpuSample *>(arg);
    int c = cpu_number();
    if (c < 0 || c >= (int)kMaxCpus)
        return;
    s->seen[c] = 1;
    s->okMask[c] = 0;
    for (unsigned i = 0; i < kPerCpuCount; i++) {
        uint32_t lo = 0, hi = 0;
        if (rdmsr_carefully(kPerCpuMsrs[i], &lo, &hi) == 0) {
            s->val[c][i] = ((uint64_t)hi << 32) | lo;
            s->okMask[c] |= 1u << i;
        }
    }
}
} // namespace

class LegionCpuProbe : public IOService {
    OSDeclareDefaultStructors(LegionCpuProbe)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
private:
    static void tick(OSObject *owner, IOTimerEventSource *src);
    void sample();
    void readMchbar();
    IOWorkLoop *workLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    CpuSample *buf = nullptr;
    uint32_t ticks = 0;
};

OSDefineMetaClassAndStructors(LegionCpuProbe, IOService)

bool LegionCpuProbe::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;
    buf = static_cast<CpuSample *>(IOMallocZero(sizeof(CpuSample)));
    if (!buf)
        return false;
    readMchbar();
    sample();
    workLoop = IOWorkLoop::workLoop();
    timer = IOTimerEventSource::timerEventSource(this, &LegionCpuProbe::tick);
    if (workLoop && timer && workLoop->addEventSource(timer) == kIOReturnSuccess)
        timer->setTimeoutMS(2000);
    registerService();
    return true;
}

void LegionCpuProbe::stop(IOService *provider) {
    if (timer) {
        timer->cancelTimeout();
        if (workLoop)
            workLoop->removeEventSource(timer);
        timer->release();
        timer = nullptr;
    }
    OSSafeReleaseNULL(workLoop);
    if (buf) {
        IOFree(buf, sizeof(CpuSample));
        buf = nullptr;
    }
    IOService::stop(provider);
}

void LegionCpuProbe::tick(OSObject *owner, IOTimerEventSource *src) {
    auto *self = OSDynamicCast(LegionCpuProbe, owner);
    if (!self)
        return;
    // the host bridge is often not yet published when the probe starts (it matches on IOResources), so MCHBAR is read
    // again on every tick; this also shows a limit that firmware or LegionCpuPower changes later
    self->readMchbar();
    self->sample();
    src->setTimeoutMS(2000);
}

void LegionCpuProbe::sample() {
    char key[48];
    bzero(buf, sizeof(CpuSample));
    mp_rendezvous_no_intrs(readPerCpu, buf);
    unsigned cpus = 0;
    for (unsigned c = 0; c < kMaxCpus; c++) {
        if (!buf->seen[c])
            continue;
        cpus++;
        for (unsigned i = 0; i < kPerCpuCount; i++) {
            snprintf(key, sizeof(key), "Cpu%02u_Msr%03X", c, kPerCpuMsrs[i]);
            if (buf->okMask[c] & (1u << i))
                setProperty(key, buf->val[c][i], 64);
            else
                setProperty(key, "missing");
        }
    }
    setProperty("CpuCount", cpus, 32);
    for (uint32_t msr : kPkgMsrs) {
        uint32_t lo = 0, hi = 0;
        snprintf(key, sizeof(key), "Pkg_Msr%03X", msr);
        if (rdmsr_carefully(msr, &lo, &hi) == 0)
            setProperty(key, ((uint64_t)hi << 32) | lo, 64);
        else
            setProperty(key, "missing");
    }
    setProperty("Ticks", ++ticks, 32);
}

// The firmware can program a second copy of PL1/PL2 in the memory controller hub (MCHBAR + 0x59A0); the CPU obeys
// the lower of the two. MCHBAR is in host bridge 00:00.0 config space at 0x48 (bit 0 = enabled).
void LegionCpuProbe::readMchbar() {
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    if (!match)
        return;
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (!it)
        return;
    IOPCIDevice *host = nullptr;
    while (OSObject *o = it->getNextObject()) {
        auto *d = OSDynamicCast(IOPCIDevice, o);
        if (d && d->getBusNumber() == 0 && d->getDeviceNumber() == 0 && d->getFunctionNumber() == 0) {
            host = d;
            break;
        }
    }
    if (!host) {
        it->release();
        setProperty("Mchbar", "no host bridge");
        return;
    }
    setProperty("HostBridgeId", host->configRead32(0), 32);
    uint64_t bar = ((uint64_t)host->configRead32(0x4C) << 32) | host->configRead32(0x48);
    it->release();
    setProperty("MchbarRaw", bar, 64);
    if (!(bar & 1)) {
        setProperty("Mchbar", "disabled");
        return;
    }
    uint64_t base = bar & 0x7FFFFFE0000ULL;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(base + 0x5000, 0x1000, kIODirectionIn);
    if (!md) {
        setProperty("Mchbar", "descriptor failed");
        return;
    }
    IOMemoryMap *map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly);
    if (map) {
        volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress() + 0x9A0);
        setProperty("Mchbar_PkgPowerLimit_59A0", ((uint64_t)p[1] << 32) | p[0], 64);
        setProperty("Mchbar_PkgEnergy_593C", *reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress() + 0x93C), 32);
        removeProperty("Mchbar");
        map->release();
    } else {
        setProperty("Mchbar", "map failed");
    }
    md->release();
}
