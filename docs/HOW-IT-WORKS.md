# How LegionCpuPower works

This is the long version: what was measured, where the two limits come from, and why the code is shaped the way it is.

## What macOS was doing

On a Core Ultra 9 275HX (8 P-cores, 16 E-cores, no Hyper-Threading) running macOS 26.7.1 with OpenCore's Comet Lake
CPUID spoof, XCPM is active (`machdep.xcpm.mode` 1, X86PlatformPlugin attached, `plugin-type` 1) and HWP is enabled
(IA32_PM_ENABLE, MSR 0x770, is 1 on every core). macOS does not know the chip is hybrid: `hw.nperflevels` is 1 and all
24 cores are one performance level.

A spin load on 1, 8 and 24 threads, with the probe reading the MSRs in the middle of each run:

| threads | throughput | package power | P-core ratio max | E-core ratio max |
|---|---|---|---|---|
| 1 | 310 Mops/s | 13 W | 33 | 38 |
| 8 | 2426 Mops/s | 26 W | 33 | 38 |
| 24 | 5666 Mops/s | 30.0 W | 33 | 33 |

Two things stand out: the 24-thread run sits at exactly 30.0 W with a power-limit reason set (MSR 0x64F) at only
80 C, and no core ever passes ratio 33 (P) or 38 (E), even with one thread at 13 W.

## Limit 1: the MCHBAR copy of the power limit

MSR 0x610 (MSR_PKG_POWER_LIMIT) reads PL1 120 W, PL2 168 W, locked. That is not the whole story. The processor also has
the package power limits in the memory controller hub's MMIO space, at MCHBAR + 0x59A0, and it enforces the lower of
the two. MCHBAR's base is in the host bridge (PCI 00:00.0) config space at 0x48, bit 0 = enabled.

On this machine MCHBAR + 0x59A0 reads 0x4280f000df80f0: PL1 30 W and PL2 30 W (in 1/8 W units from MSR 0x606), both
enabled and clamped, and the lock bit (63) is clear. The LenovoLegionLinux project documents the same behaviour on other
Legion Gen 10 models: the firmware leaves this limit low at boot, and Lenovo's software (or Intel DTT) raises it in
Windows. Linux and macOS never do.

The plugin copies MSR 0x610's limit fields (both power values, the enable, clamp and time-window bits) into MCHBAR,
without the lock bit, and writes them again if anything changes them. It uses the firmware's own MSR values, so it never
sets a limit the firmware did not already choose. With only this fix, 24 threads went from 30 W / 5666 Mops/s (taken
earlier) and 30 W / 4181 Mops/s (that boot's baseline) to 46.8 W / 6486 Mops/s, now stopped by limit 2.

## Limit 2: HWP units

XCPM writes IA32_HWP_REQUEST (MSR 0x774) on every core: min 4, max 54, desired from its own logic, EPP 0x80. The 54 is
XCPM's idea of the top ratio in 100 MHz steps (it comes from the turbo ratio MSR 0x1AD). On older CPUs HWP performance
values happened to equal 100 MHz ratios. On Arrow Lake they do not:

| | IA32_HWP_CAPABILITIES (0x771) highest / guaranteed | asking max 54 actually gives |
|---|---|---|
| P-cores | 85-87 / 44 | ratio 33 (3.3 GHz): about 61 MHz per unit |
| E-cores | 65 / 30 | ratio 38 (3.8 GHz): about 70 MHz per unit |

So "54" is read as 54 units. The P-cores stop at 3.3 GHz instead of 5.4, and the E-cores at 3.8 instead of 4.6.

### Where XCPM builds the request

In the 25G241 kernel (`/System/Library/Kernels/kernel`) there are ten places that load 0x774 into ECX. Five are writes:
two in the p-state routine that runs on every ratio change (an unnamed static function after `_xcpm_enable_pstate_ctrs`),
one each in `_xcpm_dvfs_pause`, `_xcpm_hwp_enable` and `_xcpm_init`. The hot path assembles the value from globals in
`_xcpm_pkg`: min at +0x18, max at +0x14, EPP at +0x20, and the desired ratio in a register. Every one of those paths is
guarded by `cmpl $0x0, _xcpm_hwp_enabled(%rip)`; when the flag is 0, XCPM writes IA32_PERF_CTL (0x199) instead.

Three ways to fix it were considered:

- **Overwrite the max global** with the P-cores' highest (85). XCPM recomputes it when limits change, and uses it in
  other calculations (`imul $-0x64` on it in several places, frequency vector lookups). Too easy to break something.
- **Patch the instructions** that compose the request so each field is scaled per core type. Possible, but it is a code
  patch inside an unnamed function, tied to one build, and it would still fight XCPM's own idea of the ratio.
- **Turn XCPM's HWP writes off and program HWP directly.** This is what LegionCpuPower does. With `_xcpm_hwp_enabled` at
  0, XCPM's p-state logic writes IA32_PERF_CTL, which the CPU ignores while HWP is enabled (HWP cannot be turned off
  without a reset). The plugin then writes each core's HWP_REQUEST with its own lowest and highest values, desired 0
  (let the hardware choose) and an EPP hint, the same way Windows and Linux leave HWP to the hardware. Idle states,
  thermal handling and everything else in XCPM keep working.

The plugin finds `_xcpm_hwp_enabled` with Lilu's `KernelPatcher::solveSymbol`, so it is not tied to one build's
addresses, and it checks the request every second on every core (one `mp_rendezvous_no_intrs` pass reading two MSRs per
core) in case any path writes it again.

## Result

With both fixes, at boot: 1 thread 411-424 Mops/s (P-cores reach ratio 47), 24 threads 8882 Mops/s at 83 W. Heat is the
next limit on a laptop: the package reaches 100-104 C and the CPU's thermal throttling (TjMax 105 C with an offset of 2)
lowers the clocks. The heat guard in the plugin is extra: after 30 s at 100 C or more it returns to the firmware's 30 W
until the package has been below 85 C for a minute.

## Things this does not do

- macOS still sees one performance level, so the scheduler does not know P-cores from E-cores.
- The battery profile's Low Power Mode caps XCPM's ratio further; with hwp=1 the plugin's request wins, so the
  battery-saving effect of Low Power Mode on the CPU is gone. Use EPP (`debug.legioncpu.epp`, higher = more saving) if
  battery life matters.
- No fan control. The fans follow Lenovo's EC and the Fn+Q mode.
