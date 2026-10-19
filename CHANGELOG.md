# Changelog

## 0.3.3 (2026-10-19)

- Fan control for the Legion 5 15IAX10: `debug.legioncpu.fan` (auto, full, custom curve, low) and
  `debug.legioncpu.fancurve`, through Lenovo's WMI methods, switching to Custom power mode where the EC needs it.
  No power-mode change in the first two minutes after boot (it disturbed the NVIDIA display bring-up).
- LegionFan menu bar app and boot-time LaunchDaemon (`legionfan/`).
- Heat guard steps down instead of dropping to the firmware's 30 W: measured package power minus 10 W after each minute
  at 100 C or more, floor 45 W, back up 5 W per 30 s below 90 C. `debug.legioncpu.guard=0` turns it off.
- Package power in watts in `debug.legioncpu.status`.

## 0.2.0 (2026-10-19)

- Both fixes on at boot. Boot-args `-legioncpunohwp` and `-legioncpunopl` turn one off.
- Heat guard steps back up: after 30 s at 100 C or more the firmware's power limit returns until the package has been
  below 85 C for a minute, then the raised limit applies again.

## 0.1.0 (2026-10-19)

- First version: per-core HWP requests (`debug.legioncpu.hwp`) and the MCHBAR power limit copied from MSR 0x610
  (`debug.legioncpu.pl`), both switched at runtime and off at boot.
- LegionCpuProbe 1.1 (read-only MSR and MCHBAR probe) and `tools/cpuload.py` for measuring.
