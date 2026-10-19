# LegionCpuPower

> **Heads up:** a good portion of this code is AI generated. I do understand how it works,
> and I'm happy to explain any of it. If something looks wrong or you have a question,
> open an issue or ping me on Discord (`watchthelight`).

A Lilu plugin that lets an Intel Core Ultra 200HX (Arrow Lake-HX) run at its real clocks and power under macOS Tahoe. Two
things hold it back on a Hackintosh, and the plugin fixes both:

1. **A hidden 30 W power limit.** The CPU obeys two package power limits: MSR 0x610 and a copy in the memory controller
   hub (MCHBAR + 0x59A0). On the Lenovo Legion 5 15IAX10 the firmware sets MSR 0x610 to 120 W / 168 W but leaves the
   MCHBAR copy at 30 W / 30 W. Lenovo's Windows software raises it after boot; nothing does that under macOS, so every
   core shares 30 W. The plugin copies MSR 0x610's limits into MCHBAR, unless the firmware has locked it.
2. **Clocks capped by the wrong HWP units.** macOS (XCPM) programs Speed Shift (IA32_HWP_REQUEST) with maximum 54,
   meaning 5.4 GHz in 100 MHz steps. On Arrow Lake's hybrid cores the HWP fields are in per-core performance units
   (about 61 MHz on the P-cores and 70 MHz on the E-cores of a 275HX), so 54 stops the P-cores at 3.3 GHz and the
   E-cores at 3.8 GHz. The plugin takes the HWP request over and gives each core its own lowest-to-highest range.

## Results

Lenovo Legion 5 15IAX10 (Core Ultra 9 275HX), macOS 26.7.1 (25G241), OpenCore 1.0.8, Lilu 1.7.2, on AC. The load is
`tools/spin.c` (integer work per thread), measured with `tools/cpuload.py`:

| load | before | with the plugin | change |
|---|---|---|---|
| 1 thread | 310 Mops/s, P-core ratio max 33 | 411-424 Mops/s, P-core ratio max 47 | +33-37 % |
| 8 threads | 2426 Mops/s, 26 W | 3377 Mops/s, 60 W | +39 % |
| 24 threads | 5666 Mops/s, 30 W | 8882 Mops/s, 83 W | +57 % |

On this laptop heat is now the limit: with all cores busy the package reaches 100-104 C and the CPU's own thermal
throttling takes over before 120 W or 5.4 GHz is reached. Better cooling (a stand, Lenovo's Performance mode with
Fn+Q for the louder fan curve) gives more.

## Requirements

- Intel Arrow Lake: CPUID family 6, model 0xC5 or 0xC6. The plugin reads the model with the CPUID instruction, so an
  OpenCore `Cpuid1Data` spoof does not hide it. On any other CPU it loads and does nothing.
- macOS Tahoe (Darwin 25). The HWP fix needs the kernel symbol `_xcpm_hwp_enabled`; if it is missing, that fix stays off.
- XCPM active (`sysctl machdep.xcpm.mode` = 1), with `plugin-type` = 1 on the first CPU (SSDT-PLUG or SSDT-PLUG-ALT).
- Lilu 1.7.x.

## Install

1. Download `LegionCpuPower.kext` from the releases (or build it, below) and put it in `EFI/OC/Kexts`.
2. Add it to `Kernel > Add` after Lilu, with `MinKernel` 25.0.0 and `MaxKernel` 25.99.99.
3. Reboot. Check it with `sysctl debug.legioncpu.status`:

```
cpu arrow lake, xcpm hwp flag 0, hwp 1 (rewrites 24), pl 1 (heat holds 0), epp 32, pkg 61 C, mchbar mapped
```

If it says `MCHBAR power limit locked`, your firmware locked the copy and only the HWP fix applies.

## Switches

Both fixes are on at boot. They can be changed at runtime, and a reboot always returns to the boot defaults.

| sysctl | meaning |
|---|---|
| `debug.legioncpu.hwp` | 1 = per-core HWP requests from this plugin, 0 = back to XCPM |
| `debug.legioncpu.pl` | 1 = MCHBAR limit follows MSR 0x610, 0 = the firmware's value |
| `debug.legioncpu.epp` | energy/performance hint written with hwp=1: 0 performance ... 255 power saving (default 32) |
| `debug.legioncpu.status` | read-only state |

Example: `sudo sysctl -w debug.legioncpu.pl=0`.

Boot-args: `-legioncpunohwp` and `-legioncpunopl` turn one fix off, `-legioncpuoff` disables the plugin.

**Heat guard.** If the package stays at 100 C or more for 30 s, the plugin puts the firmware's power limit back until the
package has been below 85 C for a minute, then raises it again. It never touches TjMax or any thermal register; the
CPU's own thermal protection stays in charge.

## What it changes, exactly

- MCHBAR + 0x59A0 (package power limit 1/2): set to MSR 0x610's value without the lock bit. Restored on `pl=0`.
- IA32_HWP_REQUEST (MSR 0x774) on every core: min = lowest, max = highest from IA32_HWP_CAPABILITIES (0x771),
  desired 0 (hardware decides), EPP from the sysctl. Checked once a second and written again if something changed it.
- The kernel variable `_xcpm_hwp_enabled` set to 0 while hwp=1, so XCPM stops writing HWP requests (it writes
  IA32_PERF_CTL instead, which the CPU ignores while HWP is enabled). Set back to 1 on `hwp=0`.

Nothing is written to disk or NVRAM.

## The probe

`probe/` is LegionCpuProbe, a read-only kext used to find all this. Every 2 s it publishes the relevant MSRs per core
(HWP capabilities and requests, current ratio, temperature, limit reasons), the package MSRs (power limits, energy,
TjMax, turbo ratios) and the MCHBAR power limit to the IORegistry (`ioreg -r -c LegionCpuProbe`). `tools/cpuload.py`
reads it while `tools/spin` runs and prints throughput, package watts, per-core-type clocks and limit reasons.

## Build

```
tools/build-mac.sh          # out/LegionCpuPower.kext (clones Lilu 1.7.2 into ./Lilu on first run)
probe/build-mac.sh          # probe/out/LegionCpuProbe.kext
cc -O2 -o tools/spin tools/spin.c
```

Command Line Tools only, no Xcode project.

## How it works

[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) has the measurements, the XCPM disassembly that showed where the request is
built, and why the plugin turns XCPM's HWP path off instead of patching it.

## Licence

ISC, see [LICENSE](LICENSE). Uses [Lilu](https://github.com/acidanthera/Lilu) (BSD-3-Clause) at build and run time.
