# LegionCpuPower

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

It also controls the fans of the Legion 5 15IAX10 (full speed, a low curve or your own curve) through Lenovo's own
firmware methods, with a menu bar app, LegionFan, to switch between them.

## Results

Lenovo Legion 5 15IAX10 (Core Ultra 9 275HX), macOS 26.7.1 (25G241), OpenCore 1.0.8, Lilu 1.7.2, on AC. The load is
`tools/spin.c` (integer work per thread), measured with `tools/cpuload.py`:

| load | before | with the plugin | change |
|---|---|---|---|
| 1 thread | 310 Mops/s, P-core ratio max 33 | 411-424 Mops/s, P-core ratio max 47 | +33-37 % |
| 8 threads | 2426 Mops/s, 26 W | 3377 Mops/s, 60 W | +39 % |
| 24 threads | 5666 Mops/s, 30 W | 8882 Mops/s, 83 W | +57 % |

On this laptop heat is now the limit: with all cores busy the package reaches 100-104 C and the CPU's own thermal
throttling takes over before 120 W or 5.4 GHz is reached. Better cooling (a stand, full-speed fans, below) gives more.

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
| `debug.legioncpu.sched` | 1 = the scheduler prefers the two favoured P-cores and avoids the E-cores, 0 = stock (see below) |
| `debug.legioncpu.guard` | 1 = heat guard (below), 0 = only the CPU's own thermal throttling |
| `debug.legioncpu.fan` | 0 auto (firmware curve), 1 full speed, 2 custom curve, 3 low |
| `debug.legioncpu.fancurve` | the ten points of the custom curve, fan levels 1-10, e.g. `2,2,3,4,5,6,7,8,9,10` |
| `debug.legioncpu.status` | read-only state |

Example: `sudo sysctl -w debug.legioncpu.pl=0`.

Boot-args: `-legioncpunohwp`, `-legioncpunopl` and `-legioncpunofan` turn one part off, `-legioncpuoff` disables the
plugin.

**Heat guard.** After a minute at 100 C or more, the plugin caps the power limit at the package power it measured minus
10 W (never below 45 W), and again after every further minute that hot. Each 30 s below 90 C raises the cap by 5 W
until it is back at the firmware's limits. It never touches TjMax or any thermal register; the CPU's own thermal
protection stays in charge. Set `debug.legioncpu.guard=0` for benchmarks.

**Scheduler preference (0.4.x).** macOS sees one performance level, so a single thread can land on an E-core. With
`sched=1` the plugin writes the scheduler's two per-processor-set masks (the ones only Apple Silicon's Edge scheduler
normally sets): preferred = the favoured P-cores (highest HWP value, cpus 10 and 22 here), avoid = the E-cores. It checks
the structure offsets against the kernel's instructions first and stays off if anything differs. Measured with the spin
load: 1 thread +10-13 %, 4 threads +13 %, 8 threads +7 %, 24 threads +1 %. Status: it booted and measured fine on the
15IAX10 (0.4.1), but one desktop freeze followed a session with it on; the freeze matches an NVIDIA driver bug that is
not ours (a thread_call pileup), yet the scheduler path has not been run for long since, so treat `sched` as
experimental. The author runs with it off.

## Fans (Legion 5 15IAX10)

Fan modes go through Lenovo's WMI methods on the `\_SB.GZFD` ACPI device, the same calls Lenovo Vantage makes on
Windows: full speed is feature 0x04020000 of `WMAE`, the curve is `WMAB` methods 5 and 6 (ten fan levels in EC RAM).
On this EC both only take effect in Lenovo's Custom power mode, so modes 1-3 switch the laptop to Custom (`WMAA` 0x2C
with 0xFF, on AC only) and auto puts the previous mode back. The plugin waits two minutes after boot before changing
the power mode: the firmware notifies the GPU on a mode change, and doing that while the NVIDIA driver was bringing up
the display left it without acceleration. In modes 2 and 3, 10 s at 95 C or more switches to full speed until the
package has been below 80 C for a minute.

Measured: full speed 5500-5700 rpm, the firmware curve 3300-4300 rpm at the same temperatures.

**LegionFan** (`legionfan/`) is a menu bar app for this: Auto, Low, Full speed, Custom curve and a ten-slider curve
editor, with the fan speeds in the menu. A root LaunchDaemon applies the saved choice at boot, before login; the app
starts at login. `legionfan/build.sh`, then `sudo legionfan/install.sh` (`--remove` uninstalls).

LegionFan 0.3 also has a CPU profile (Performance, Balanced, Battery saver: EPP 0, 32, 128), switches for the P-core
preference and the heat guard, and a glass-style panel (needs macOS 26).

## What it changes, exactly

- MCHBAR + 0x59A0 (package power limit 1/2): set to MSR 0x610's value without the lock bit. Restored on `pl=0`.
- IA32_HWP_REQUEST (MSR 0x774) on every core: min = lowest, max = highest from IA32_HWP_CAPABILITIES (0x771),
  desired 0 (hardware decides), EPP from the sysctl. Checked once a second and written again if something changed it.
- The kernel variable `_xcpm_hwp_enabled` set to 0 while hwp=1, so XCPM stops writing HWP requests (it writes
  IA32_PERF_CTL instead, which the CPU ignores while HWP is enabled). Set back to 1 on `hwp=0`.

- With a fan mode other than auto: Lenovo's power mode (Custom), the EC's full-speed bit and its custom fan table,
  through the firmware's own ACPI methods. Auto restores the previous power mode and the fan table found at boot.

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
